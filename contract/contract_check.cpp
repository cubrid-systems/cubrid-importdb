// cubrid-importdb -- upstream contract check
//
// One program, one job: assert that the CUBRID install this was built against
// still provides everything an out-of-tree utility needs. It is the artifact
// both CI lanes run -- the engine repo runs it against a freshly built engine
// (so an upstream change fails there), and this repo runs it against released
// installs (so drift is caught even when the engine lane does not run).
//
// Three layers, in order of how early they fail:
//   compile : the installed headers must still DECLARE the surface
//   link    : libcubridcs must still EXPORT it
//   runtime : the substitutes must still BEHAVE (SQL forms, parameter reads,
//             catalog reads, the cub_admin argv contract)
//
// Usage: contract_check <database> [objects-file]
// Exit : 0 all pass, 1 a check failed, 2 usage/connect error
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>

#include "dbi.h"                // the only header set this repo may use
#include "db_set_function.h"

// Not installed: src/compat/db_client_type.hpp. Replicated as a literal, which
// is exactly why check 12 exists.
static const int OOT_DB_CLIENT_TYPE_ADMIN_UTILITY = 7;

static int g_pass = 0, g_fail = 0;

static void
report (const char *name, bool ok, const std::string &detail)
{
  std::printf ("%-46s %s%s%s\n", name, ok ? "PASS" : "FAIL",
               detail.empty () ? "" : "  ", detail.c_str ());
  if (ok) g_pass++; else g_fail++;
}

// ---------------------------------------------------------------- helpers
static bool
run_sql (const char *sql, std::string &err)
{
  DB_QUERY_RESULT *r = NULL;
  DB_QUERY_ERROR qe;
  if (db_execute (sql, &r, &qe) < 0)
    {
      const char *e = db_error_string (3);
      err = e ? e : "(no message)";
      return false;
    }
  if (r) db_query_end (r);
  err.clear ();
  return true;
}

static int
scalar (const char *sql, std::string &out, std::string &err)
{
  DB_QUERY_RESULT *result = NULL;
  DB_QUERY_ERROR qe;
  out.clear ();
  if (db_execute (sql, &result, &qe) < 0)
    {
      const char *e = db_error_string (3);
      err = e ? e : "(no message)";
      return -1;
    }
  if (db_query_first_tuple (result) == DB_CURSOR_SUCCESS)
    {
      DB_VALUE v;
      if (db_query_get_tuple_value (result, 0, &v) == NO_ERROR)
        {
          DB_TYPE t = db_value_type (&v);
          if (DB_IS_NULL (&v))           out = "NULL";
          else if (t == DB_TYPE_INTEGER) out = std::to_string (db_get_int (&v));
          else if (t == DB_TYPE_BIGINT)  out = std::to_string (db_get_bigint (&v));
          else
            {
              const char *s = db_get_string (&v);
              out = s ? s : "";
            }
          db_value_clear (&v);
        }
    }
  db_query_end (result);
  return 0;
}

static int
count_rows (const char *sql, std::string &err)
{
  DB_QUERY_RESULT *r = NULL;
  DB_QUERY_ERROR qe;
  if (db_execute (sql, &r, &qe) < 0)
    {
      const char *e = db_error_string (3);
      err = e ? e : "(no message)";
      return -1;
    }
  int n = 0, rc = db_query_first_tuple (r);
  while (rc == DB_CURSOR_SUCCESS) { n++; rc = db_query_next_tuple (r); }
  db_query_end (r);
  return n;
}

// ---------------------------------------------------------------- checks
int
main (int argc, char **argv)
{
  if (argc < 2)
    {
      std::printf ("usage: %s <database> [objects-file]\n", argv[0]);
      return 2;
    }
  const char *db = argv[1];
  const char *objfile = (argc > 2) ? argv[2] : NULL;
  const char *cubrid = getenv ("CUBRID");
  std::string err, val;

  std::printf ("cubrid-importdb upstream contract check\n");
  std::printf ("  database : %s\n", db);
  std::printf ("  CUBRID   : %s\n\n", cubrid ? cubrid : "(unset)");

  // 1. the admin-utility client type is still accepted
  // CONTRACT_USER lets the negative test drive the same checks as a non-DBA,
  // which is how the "AU window is unnecessary" assumption is falsified rather
  // than merely asserted (checks 03 and 05 must fail there).
  const char *user = getenv ("CONTRACT_USER");
  const char *pass = getenv ("CONTRACT_PASSWORD");
  if (user == NULL) user = "dba";
  db_set_client_type (OOT_DB_CLIENT_TYPE_ADMIN_UTILITY);
  bool connected = (db_login (user, pass) == NO_ERROR
                    && db_restart (argv[0], 0, db) == NO_ERROR);
  report ("01 connect as ADMIN_UTILITY client type", connected,
          connected ? std::string ("user=") + user : db_error_string (3));
  if (!connected)
    {
      std::printf ("\ncannot continue without a connection\n");
      return 2;
    }

  // 2. catalog VIEW read -- the Graph builder's class inventory
  int n = count_rows ("SELECT class_name FROM db_class WHERE is_system_class='NO'", err);
  report ("02 db_class view readable", n >= 0, n >= 0 ? "" : err);

  // 3. UNDERLYING system class read, with authorization ENABLED.
  //    This is the check that says the AU window is not needed. If a future
  //    engine tightens this, the utility must go back to an AU-equivalent.
  n = count_rows ("SELECT name, class_name FROM _db_serial", err);
  report ("03 _db_serial readable as DBA, AU enabled", n >= 0, n >= 0 ? "" : err);

  // 4. the db_serial VIEW must still NOT be a substitute -- it omits
  //    AUTO_INCREMENT serials, which is why check 03 has to read the class.
  //    Informational: if this ever equals the class count, the view became usable.
  int nview = count_rows ("SELECT name, class_name FROM db_serial", err);
  {
    // The fixture has one standalone serial and one AUTO_INCREMENT class, so the
    // class must see strictly more rows than the view. If this ever fails the
    // view became usable (good news -- drop the _db_serial read) or AI serials
    // moved somewhere else (bad news -- the graph loses serial edges).
    bool ok = (n > 0 && nview >= 0 && nview < n);
    char d[160];
    std::snprintf (d, sizeof (d), "_db_serial=%d db_serial=%d%s", n, nview,
                   ok ? " (view narrower, as the design assumes)" : "  <-- assumption changed");
    report ("04 db_serial view is narrower than the class", ok, d);
  }

  // 5. db_user.groups is still a SET the DBA gate can iterate
  bool gate_ok = false;
  std::string who;
  {
    DB_QUERY_RESULT *result = NULL;
    DB_QUERY_ERROR qe;
    if (db_execute ("SELECT name, groups FROM db_user WHERE name = CURRENT_USER",
                    &result, &qe) >= 0)
      {
        if (db_query_first_tuple (result) == DB_CURSOR_SUCCESS)
          {
            DB_VALUE nv, gv;
            if (db_query_get_tuple_value (result, 0, &nv) == NO_ERROR && !DB_IS_NULL (&nv))
              {
                const char *s = db_get_string (&nv);
                who = s ? s : "";
                if (who == "DBA") gate_ok = true;
              }
            if (db_query_get_tuple_value (result, 1, &gv) == NO_ERROR && !DB_IS_NULL (&gv))
              {
                DB_COLLECTION *col = db_get_set (&gv);
                int sz = col ? db_col_size (col) : 0;
                for (int i = 0; i < sz; i++)
                  {
                    DB_VALUE ev;
                    if (db_col_get (col, i, &ev) == NO_ERROR && !DB_IS_NULL (&ev))
                      {
                        const char *g = db_get_string (&ev);
                        if (g && std::strcmp (g, "DBA") == 0) gate_ok = true;
                      }
                    db_value_clear (&ev);
                  }
              }
            db_value_clear (&nv);
            db_value_clear (&gv);
          }
        db_query_end (result);
      }
  }
  report ("05 DBA gate via db_user.groups SET", gate_ok, "user=" + who);

  // 6. db_get_system_parameters -- the HA_DISABLED and prm_get_* substitute
  {
    char buf[4096];
    std::strcpy (buf, "ha_mode");
    int rc = db_get_system_parameters (buf, (int) sizeof (buf));
    bool ok = (rc == NO_ERROR && std::strstr (buf, "ha_mode") != NULL);
    report ("06 db_get_system_parameters(ha_mode)", ok, ok ? buf : "rc/parse failed");
  }
  {
    char buf[4096];
    std::strcpy (buf, "loaddb_worker_count");
    int rc = db_get_system_parameters (buf, (int) sizeof (buf));
    bool ok = (rc == NO_ERROR && std::strstr (buf, "loaddb_worker_count") != NULL);
    report ("07 db_get_system_parameters(worker_count)", ok, ok ? buf : "rc/parse failed");
  }

  // 8-10. the SQL forms that replace sm_update_statistics
  bool ok8 = run_sql ("CREATE TABLE cc_probe (a INTEGER, b VARCHAR(20));", err);
  if (ok8) run_sql ("INSERT INTO cc_probe VALUES (1,'x'),(2,'y');", err);
  report ("08 DDL via db_execute (CREATE)", ok8, ok8 ? "" : err);
  bool ok9 = run_sql ("UPDATE STATISTICS ON cc_probe;", err);
  report ("09 UPDATE STATISTICS ON <cls>", ok9, ok9 ? "" : err);
  bool ok9b = run_sql ("UPDATE STATISTICS ON cc_probe WITH FULLSCAN;", err);
  report ("10 UPDATE STATISTICS ... WITH FULLSCAN", ok9b, ok9b ? "" : err);
  bool ok9c = run_sql ("UPDATE STATISTICS ON ALL CLASSES;", err);
  report ("11 UPDATE STATISTICS ON ALL CLASSES", ok9c, ok9c ? "" : err);

  // 12. the constraint lifecycle as SQL DDL (strip / rebuild / fkdefine shape)
  bool okpk = run_sql ("ALTER TABLE cc_probe ADD CONSTRAINT cc_pk PRIMARY KEY (a);", err);
  bool okdrop = okpk && run_sql ("ALTER TABLE cc_probe DROP CONSTRAINT cc_pk;", err);
  bool oktr = okdrop && run_sql ("TRUNCATE cc_probe;", err);
  report ("12 ALTER ADD/DROP CONSTRAINT + TRUNCATE", okpk && okdrop && oktr,
          (okpk && okdrop && oktr) ? "" : err);

  // 13. db_open_buffer multi-statement session -- the define phase's path
  //     (and the one whose statements REPLICATE, unlike a FILE session)
  {
    DB_SESSION *s = db_open_buffer ("CREATE TABLE cc_probe2 (x INTEGER); DROP TABLE cc_probe2;");
    int executed = 0;
    if (s != NULL)
      {
        int stmt;
        while ((stmt = db_compile_statement (s)) > 0)
          {
            DB_QUERY_RESULT *sr = NULL;
            if (db_execute_statement (s, stmt, &sr) < 0) break;
            if (sr) db_query_end (sr);
            executed++;
          }
        db_close_session (s);
      }
    report ("13 db_open_buffer multi-statement session", executed == 2,
            "statements executed=" + std::to_string (executed));
  }

  // 14. trigger suppression + transaction control
  db_disable_trigger ();
  bool okc = (db_commit_transaction () == NO_ERROR);
  report ("14 db_disable_trigger + commit", okc, okc ? "" : db_error_string (3));

  // 15. the cub_admin argv pass-through contract and the data phase.
  //     `cub_admin loaddb ...` must stay equivalent to `cubrid loaddb ...` --
  //     it is what makes the loader a DIRECT child, which is what lets
  //     PR_SET_PDEATHSIG reach it.
  if (objfile != NULL && cubrid != NULL)
    {
      run_sql ("DROP TABLE cc_load;", err);
      run_sql ("CREATE TABLE t_narrow (a INTEGER, b INTEGER, c INTEGER, d INTEGER,"
               " e INTEGER, f INTEGER, g INTEGER, h INTEGER);", err);
      db_commit_transaction ();
      std::string bin = std::string (cubrid) + "/bin/cub_admin";
      std::fflush (stdout);
      pid_t pid = fork ();
      if (pid == 0)
        {
          prctl (PR_SET_PDEATHSIG, SIGKILL);
          if (freopen ("/dev/null", "w", stdout) == NULL) _exit (126);
          if (freopen ("/dev/null", "w", stderr) == NULL) _exit (126);
          execl (bin.c_str (), "cub_admin", "loaddb", "-C", "-u", "dba",
                 "-d", objfile, db, (char *) NULL);
          _exit (127);
        }
      int status = 0;
      waitpid (pid, &status, 0);
      int ex = WIFEXITED (status) ? WEXITSTATUS (status) : -1;
      report ("15 exec cub_admin loaddb -C (argv contract)", ex == 0,
              "child exit=" + std::to_string (ex));
      db_commit_transaction ();
      if (scalar ("SELECT COUNT(*) FROM t_narrow", val, err) == 0)
        report ("16 rows visible after the child's load", val != "0" && !val.empty (),
                "count=" + val);
      else
        report ("16 rows visible after the child's load", false, err);
      run_sql ("DROP TABLE t_narrow;", err);
    }
  else
    std::printf ("%-46s SKIP  no objects file / $CUBRID\n",
                 "15 exec cub_admin loaddb -C (argv contract)");

  run_sql ("DROP TABLE cc_probe;", err);
  db_commit_transaction ();
  db_shutdown ();

  std::printf ("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
