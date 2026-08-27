/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * import_validate.cpp - Stage 4 (FK re-validation, step 4 of the constraint lifecycle)
 *
 * Validates every FK edge of the loaded target by a set-based anti-join, BEFORE
 * the FK is defined (FK define is WU-34): CUBRID's ADD FOREIGN KEY does not check
 * the existing rows, so this phase IS the validation (10-design.md §6 / §4.5).
 * Per edge child -> parent it runs
 *   SELECT CAST(c.<pk/fk col> AS VARCHAR) ... FROM [child] AS c
 *   WHERE <every fk col> IS NOT NULL
 *     AND NOT EXISTS (SELECT 1 FROM [parent] AS p WHERE p.<pk col> = c.<fk col> [AND ...])
 * over the same public single-statement client primitive the Graph builder reads
 * the catalog with (db_execute), authorization disabled for the DBA-group path.
 * The result rows are exactly the orphan child rows. The child FK columns are
 * paired with the parent PK columns POSITIONALLY (the mapping the Graph builder
 * captured at snapshot: fk_edge.child_columns <-> parent_pk_columns), which is
 * why FK and parent-PK column names need not match; a composite FK pairs several
 * columns. The selected identity/value columns are CAST to VARCHAR so every type
 * (int, date, ...) reads back uniformly as a string. loaddb is not touched.
 *
 * Policy (WU-10 D4): default fail-fast stops at the first violated edge (recording
 * only its offenders); --continue validates every non-withheld edge and
 * enumerates every offending row. Either way any orphan makes the run exit
 * non-zero. An edge whose parent PK the Rebuild phase withheld cannot be
 * validated and is skipped (not a new failure). On a violation the offenders are
 * written to the exceptions artifact in the importdb directory (§4.4). The caller
 * owns the transaction (import_session.hpp): validate_fks () neither commits nor
 * aborts.
 */

#include "import_validate.hpp"

#include "db.h"
#include "dbtype.h"
#include "authenticate.h"
#include "environment_variable.h"	/* envvar_bindir_file - locate the csql binary (WU-41) */

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
  /* Basename of the importdb-written exceptions artifact inside the importdb
   * directory. Named like the manifest (importdb.<role>) so a later Discovery
   * re-scan (resume) tolerates it and it never collides with a dump artifact. */
  const char *const EXCEPTIONS_BASENAME = "importdb.exceptions";

  const int EXCEPTIONS_FORMAT_VERSION = 1;

  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  std::string
  path_join (const std::string &dir, const std::string &name)
  {
    if (dir.empty () || dir.back () == '/')
      {
	return dir + name;
      }
    return dir + "/" + name;
  }

  std::string
  utc_timestamp ()
  {
    time_t now = time (NULL);
    struct tm tm_buf;
    char buf[32] = "";
    if (gmtime_r (&now, &tm_buf) != NULL)
      {
	strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
      }
    return std::string (buf);
  }

  /* Comma-separated join of a string list (column-name lists in diagnostics). */
  std::string
  join_list (const std::vector<std::string> &items)
  {
    std::string out;
    for (size_t i = 0; i < items.size (); i++)
      {
	if (i != 0)
	  {
	    out += ", ";
	  }
	out += items[i];
      }
    return out;
  }

  /* Read one column of the current tuple as a string (the anti-join CASTs every
   * selected column to VARCHAR, so db_get_string yields the value directly; a
   * NULL - not expected for PK/filtered-FK columns - reads as "NULL"). */
  std::string
  cell_string (DB_QUERY_RESULT *result, int idx)
  {
    DB_VALUE value;
    std::string out = "NULL";

    if (db_query_get_tuple_value (result, idx, &value) != NO_ERROR)
      {
	return "";
      }
    if (!DB_IS_NULL (&value))
      {
	const char *s = db_get_string (&value);
	out = (s != NULL) ? std::string (s) : std::string ("");
      }
    db_value_clear (&value);
    return out;
  }

  /*
   * Run one SELECT and collect ncols string columns per row into rows. Returns
   * NO_ERROR on success (rows appended), or the negative db_execute error code.
   * Mirrors the Graph builder's catalog-read primitive.
   */
  int
  run_query (const std::string &sql, int ncols, std::vector<std::vector<std::string>> &rows)
  {
    DB_QUERY_RESULT *result = NULL;
    DB_QUERY_ERROR query_error;

    int res = db_execute (sql.c_str (), &result, &query_error);
    if (res < 0)
      {
	return res;
      }
    if (res > 0)
      {
	int pos = db_query_first_tuple (result);
	while (pos == DB_CURSOR_SUCCESS)
	  {
	    std::vector<std::string> row;
	    row.reserve (ncols);
	    for (int i = 0; i < ncols; i++)
	      {
		row.push_back (cell_string (result, i));
	      }
	    rows.push_back (std::move (row));
	    pos = db_query_next_tuple (result);
	  }
      }
    db_query_end (result);
    return NO_ERROR;
  }

  /*
   * The shared "FROM [child] AS c WHERE <every fk col> IS NOT NULL AND NOT EXISTS
   * (parent match)" tail of the anti-join for one FK edge (positional FK col <->
   * parent PK col equality; bracket-quoted; aliased so equal column names on the
   * two sides stay unambiguous). Both the enumeration SELECT (build_antijoin) and
   * the parallel count SELECT (build_antijoin_count) use THIS same tail, so the
   * two paths test byte-identical predicates and can never disagree on clean vs
   * violated.
   */
  std::string
  antijoin_from_where (const cubimport::fk_edge &e)
  {
    std::string sql = " FROM [" + e.child + "] AS c WHERE ";
    for (size_t i = 0; i < e.child_columns.size (); i++)
      {
	sql += (i == 0 ? "" : " AND ");
	sql += "c.[" + e.child_columns[i] + "] IS NOT NULL";
      }

    sql += " AND NOT EXISTS (SELECT 1 FROM [" + e.parent + "] AS p WHERE ";
    for (size_t i = 0; i < e.child_columns.size (); i++)
      {
	sql += (i == 0 ? "" : " AND ");
	sql += "p.[" + e.parent_pk_columns[i] + "] = c.[" + e.child_columns[i] + "]";
      }
    sql += ")";
    return sql;
  }

  /*
   * Build the enumeration anti-join SELECT for one FK edge: the select list is the
   * child's PK columns (row identity) then the FK columns (offending value), each
   * CAST to VARCHAR so they read back as strings, over the shared FROM+WHERE. The
   * result rows are exactly the orphan child rows.
   */
  std::string
  build_antijoin (const cubimport::fk_edge &e)
  {
    std::string sql = "SELECT ";
    bool first = true;
    for (const std::string &col : e.child_pk_columns)
      {
	sql += (first ? "" : ", ");
	sql += "CAST(c.[" + col + "] AS VARCHAR)";
	first = false;
      }
    for (const std::string &col : e.child_columns)
      {
	sql += (first ? "" : ", ");
	sql += "CAST(c.[" + col + "] AS VARCHAR)";
	first = false;
      }
    sql += antijoin_from_where (e);
    return sql;
  }

  /* The count-only anti-join for one FK edge (parallel clean/violated decision):
   * SELECT count(*) over the SAME FROM+WHERE the enumeration uses. 0 = clean. */
  std::string
  build_antijoin_count (const cubimport::fk_edge &e)
  {
    return "SELECT count(*)" + antijoin_from_where (e);
  }

  /* Join column names with the matching values from one result row (starting at
   * offset) as "col=value, ..." - the child key or the FK values of one orphan. */
  std::string
  join_pairs (const std::vector<std::string> &cols, const std::vector<std::string> &row, size_t offset)
  {
    std::string out;
    for (size_t i = 0; i < cols.size (); i++)
      {
	if (i != 0)
	  {
	    out += ", ";
	  }
	out += cols[i] + "=" + ((offset + i < row.size ()) ? row[offset + i] : std::string ());
      }
    return out;
  }

  /* Locate an FK edge by (child, parent, name) for its column lists. */
  const cubimport::fk_edge *
  find_edge (const cubimport::dependency_graph &graph, const cubimport::fk_edge_result &r)
  {
    for (const cubimport::fk_edge &e : graph.fk_edges)
      {
	if (e.child == r.child && e.parent == r.parent && e.name == r.name)
	  {
	    return &e;
	  }
      }
    return NULL;
  }

  /*
   * Render + write the exceptions artifact: a header (policy + tallies) then, per
   * violated edge, its column lists and one "orphan:" line per offending child
   * row. Human-readable and regular so an operator can repair from it. Returns
   * true on success; on failure leaves errno set for the caller's diagnostic.
   */
  bool
  write_exceptions (const std::string &path, const cubimport::import_set &iset,
		    const cubimport::dependency_graph &graph, const cubimport::validate_summary &summary,
		    bool continue_on_error)
  {
    std::ostringstream os;
    os << "# CUBRID importdb FK re-validation exceptions (format v" << EXCEPTIONS_FORMAT_VERSION << ")\n";
    os << "# Written and owned by importdb; do not edit by hand.\n";
    os << "# Each 'orphan' line is a child row whose foreign-key value has no matching parent primary key.\n";
    os << "# An operator repairs the data from this file; the FK on these edges is left undefined.\n";
    os << "generator: importdb\n";
    os << "created: " << utc_timestamp () << "\n";
    os << "database: " << iset.database_name << "\n";
    os << "policy: " << (continue_on_error ? "continue" : "fail-fast") << "\n";
    os << "violated_edges: " << summary.violated_edges << "\n";
    os << "total_orphans: " << summary.total_orphans << "\n";
    os << "\n";

    for (const cubimport::fk_edge_result &r : summary.edges)
      {
	if (r.skipped || r.orphans == 0)
	  {
	    continue;
	  }
	const cubimport::fk_edge *e = find_edge (graph, r);
	os << "[edge] " << r.child << " -> " << r.parent << " (" << r.name << ")\n";
	os << "child_key_columns: " << ((e != NULL && !e->child_pk_columns.empty ()) ? join_list (e->child_pk_columns)
					: std::string ("(none)")) << "\n";
	os << "fk_columns: " << (e != NULL ? join_list (e->child_columns) : std::string ()) << "\n";
	os << "orphans: " << r.orphans << "\n";
	for (const cubimport::fk_orphan &o : summary.orphans)
	  {
	    if (o.child == r.child && o.parent == r.parent && o.fk_name == r.name)
	      {
		os << "orphan: child[" << o.child_key << "] fk[" << o.fk_values << "]\n";
	      }
	  }
	os << "\n";
      }

    const std::string content = os.str ();
    FILE *fp = fopen (path.c_str (), "w");
    if (fp == NULL)
      {
	return false;
      }
    if (fwrite (content.data (), 1, content.size (), fp) != content.size ())
      {
	int saved = errno;
	fclose (fp);
	errno = saved;
	return false;
      }
    if (fclose (fp) != 0)
      {
	return false;
      }
    return true;
  }

  /* ---- WU-41 parallel FK re-validation: bounded csql -C count-anti-join pool ---- */

  /* The csql binary path ($CUBRID/bin/csql); "csql" (PATH lookup) fallback. */
  std::string
  csql_binary ()
  {
    char path[PATH_MAX];
    if (envvar_bindir_file (path, sizeof (path), "csql") != NULL)
      {
	return std::string (path);
      }
    return "csql";
  }

  /*
   * Spawn `csql -C -u <user> [-p <pw>] -t -N -c "<count-sql>" <db>`, its output
   * redirected to log_path. argv + the C strings are built in the PARENT so the
   * child does only async-signal-safe calls (as in import_load's hardened pool).
   * Returns the child pid, or -1 on fork failure.
   */
  pid_t
  spawn_csql_count (const std::string &bin, const std::string &db, const std::string &user,
		    const std::string &password, const std::string &sql, const std::string &log_path)
  {
    std::vector<std::string> a = { "csql", "-C", "-u", user };
    /* -p exposes the password in the child's `ps` cmdline for its lifetime - the
     * accepted CUBRID CS-tool convention (see spawn_loaddb in import_load.cpp for
     * the rationale: no non-tty batch password channel exists). */
    if (!password.empty ())
      {
	a.push_back ("-p");
	a.push_back (password);
      }
    a.push_back ("-t");
    a.push_back ("-N");
    a.push_back ("-c");
    a.push_back (sql);
    a.push_back (db);

    std::vector<char *> argv;
    argv.reserve (a.size () + 1);
    for (std::string &s : a)
      {
	argv.push_back (const_cast<char *> (s.c_str ()));
      }
    argv.push_back (NULL);
    const char *bin_c = bin.c_str ();
    const char *log_c = log_path.c_str ();

    /* Upper bound for the child's pre-exec fd-close loop, resolved in the PARENT
     * so the child does only async-signal-safe calls; capped so a large
     * RLIMIT_NOFILE does not turn the loop into a million close () calls. */
    long open_max = sysconf (_SC_OPEN_MAX);
    if (open_max <= 0 || open_max > 4096)
      {
	open_max = 4096;
      }

    pid_t pid = fork ();
    if (pid < 0)
      {
	return -1;
      }
    if (pid == 0)
      {
	/* child: async-signal-safe calls only. Close every inherited descriptor
	 * above stderr FIRST - the parent is a CS client holding an open (non-
	 * CLOEXEC) server-connection socket the exec'd csql child must not keep
	 * open (mirrors dynamic_load.c's post-fork child + import_load's pool). */
	for (int i = STDERR_FILENO + 1; i < (int) open_max; ++i)
	  {
	    close (i);
	  }
	int fd = open (log_c, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0)
	  {
	    dup2 (fd, STDOUT_FILENO);
	    dup2 (fd, STDERR_FILENO);
	    close (fd);
	  }
	execvp (bin_c, argv.data ());
	_exit (127);
      }
    return pid;
  }

  /*
   * Parse the orphan count from a csql count-query log: the first all-digit line
   * (csql -t -N prints the bare count). Returns 0 (clean), 1 (violated - the
   * exact count is not needed, the serial path enumerates), or -1 when no
   * digit-only line is present (csql error / unexpected output -> serial fallback).
   */
  int64_t
  parse_count (const std::string &log_path)
  {
    FILE *fp = fopen (log_path.c_str (), "r");
    if (fp == NULL)
      {
	return -1;
      }
    char buf[512];
    int64_t result = -1;
    while (fgets (buf, sizeof (buf), fp) != NULL)
      {
	std::string line (buf);
	size_t b = line.find_first_not_of (" \t\r\n");
	if (b == std::string::npos)
	  {
	    continue;
	  }
	size_t e = line.find_last_not_of (" \t\r\n");
	std::string t = line.substr (b, e - b + 1);
	bool all_digits = true;
	for (char ch : t)
	  {
	    if (ch < '0' || ch > '9')
	      {
		all_digits = false;
		break;
	      }
	  }
	if (all_digits)
	  {
	    result = (t == "0") ? 0 : 1;
	    break;
	  }
      }
    fclose (fp);
    return result;
  }
} // namespace

namespace cubimport
{

  validate_status
  validate_fks (const import_set &iset, const dependency_graph &graph, const rebuild_summary &rebuild,
		bool continue_on_error, validate_summary &summary)
  {
    summary = validate_summary ();

    /* FK edges whose parent PK the Rebuild phase withheld cannot be validated
     * (no parent PK index); they are skipped, not counted as a new failure. */
    std::set<std::string> withheld;
    for (const withheld_fk &w : rebuild.withheld)
      {
	withheld.insert (w.name);
      }

    /* Validate in a deterministic order (child, parent, name) so the fail-fast
     * "first violated edge" is stable across runs. */
    std::vector<const fk_edge *> edges;
    for (const fk_edge &e : graph.fk_edges)
      {
	edges.push_back (&e);
      }
    std::sort (edges.begin (), edges.end (), [] (const fk_edge *a, const fk_edge *b)
    {
      if (a->child != b->child)
	{
	  return a->child < b->child;
	}
      if (a->parent != b->parent)
	{
	  return a->parent < b->parent;
	}
      return a->name < b->name;
    });

    /* As the other server-side phases do, DBA-group members run the anti-join
     * with authorization disabled. */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    bool violated_any = false;
    bool hard_error = false;

    for (const fk_edge *e : edges)
      {
	fk_edge_result r;
	r.child = e->child;
	r.parent = e->parent;
	r.name = e->name;

	if (withheld.count (e->name))
	  {
	    r.skipped = true;
	    summary.skipped_edges++;
	    summary.edges.push_back (r);
	    continue;
	  }

	/* The column mapping is captured at snapshot; a real FK edge always has
	 * matching-arity child + parent-PK columns. A malformed/missing mapping
	 * is a hard error (the anti-join cannot be built correctly). */
	if (e->child_columns.empty () || e->parent_pk_columns.empty ()
	    || e->child_columns.size () != e->parent_pk_columns.size ())
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_VALIDATE_QUERY_FAILED), e->child.c_str (), e->parent.c_str (),
				   e->name.c_str (), "missing or malformed FK-to-PK column mapping");
	    hard_error = true;
	    break;
	  }

	const int ncols = (int) (e->child_pk_columns.size () + e->child_columns.size ());
	std::vector<std::vector<std::string>> rows;
	if (run_query (build_antijoin (*e), ncols, rows) != NO_ERROR)
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_VALIDATE_QUERY_FAILED), e->child.c_str (), e->parent.c_str (),
				   e->name.c_str (), db_error_string (3));
	    hard_error = true;
	    break;
	  }

	r.orphans = (int64_t) rows.size ();
	summary.validated_edges++;
	summary.edges.push_back (r);

	if (rows.empty ())
	  {
	    continue;
	  }

	/* Violated: enumerate this edge's offenders into the summary. */
	violated_any = true;
	summary.violated_edges++;
	summary.total_orphans += r.orphans;
	const size_t pkcnt = e->child_pk_columns.size ();
	for (const std::vector<std::string> &row : rows)
	  {
	    fk_orphan o;
	    o.child = e->child;
	    o.parent = e->parent;
	    o.fk_name = e->name;
	    o.child_key = pkcnt ? join_pairs (e->child_pk_columns, row, 0) : std::string ("(no pk)");
	    o.fk_values = join_pairs (e->child_columns, row, pkcnt);
	    summary.orphans.push_back (o);
	  }

	/* Default policy is fail-fast: stop at the first violated edge. */
	if (!continue_on_error)
	  {
	    break;
	  }
      }

    AU_RESTORE (au_save);

    if (hard_error)
      {
	summary = validate_summary ();
	return validate_status::ERR_VALIDATE;
      }

    if (violated_any)
      {
	summary.exceptions_file = EXCEPTIONS_BASENAME;
	const std::string path = path_join (iset.dump_dir, EXCEPTIONS_BASENAME);
	if (!write_exceptions (path, iset, graph, summary, continue_on_error))
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_EXCEPTIONS_WRITE_FAILED), path.c_str (), strerror (errno));
	    summary = validate_summary ();
	    return validate_status::ERR_VALIDATE;
	  }

	if (!continue_on_error)
	  {
	    /* fail-fast: name the single violated edge + its orphan count. */
	    for (const fk_edge_result &r : summary.edges)
	      {
		if (!r.skipped && r.orphans > 0)
		  {
		    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_VALIDATE_EDGE_VIOLATED), (int) r.orphans, r.child.c_str (),
					   r.parent.c_str (), r.name.c_str (), path.c_str ());
		    break;
		  }
	      }
	  }
	else
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_VALIDATE_VIOLATIONS), graph.database_name.c_str (),
				   summary.violated_edges, (long) summary.total_orphans, path.c_str ());
	  }
	return validate_status::VIOLATED;
      }

    fprintf (stdout, msg (IMPORTDB_MSG_VALIDATE_COMPLETE), graph.database_name.c_str (), summary.validated_edges,
	     summary.skipped_edges);
    return validate_status::OK;
  }

  validate_status
  validate_fks_parallel (const import_set &iset, const dependency_graph &graph, const rebuild_summary &rebuild,
			 bool continue_on_error, int degree, const char *user, const char *password,
			 validate_summary &summary)
  {
    summary = validate_summary ();

    std::set<std::string> withheld;
    for (const withheld_fk &w : rebuild.withheld)
      {
	withheld.insert (w.name);
      }

    /* Worklist of edges to count in parallel (non-withheld, valid mapping). A
     * malformed mapping routes the whole phase to the serial path, which reports
     * it authoritatively; an empty worklist (all withheld / no FK) likewise lets
     * the serial path build the (skipped-only) summary. */
    std::vector<const fk_edge *> work;
    for (const fk_edge &e : graph.fk_edges)
      {
	if (withheld.count (e.name))
	  {
	    continue;
	  }
	if (e.child_columns.empty () || e.parent_pk_columns.empty ()
	    || e.child_columns.size () != e.parent_pk_columns.size ())
	  {
	    return validate_fks (iset, graph, rebuild, continue_on_error, summary);
	  }
	work.push_back (&e);
      }
    if (work.empty ())
      {
	return validate_fks (iset, graph, rebuild, continue_on_error, summary);
      }

    const std::string bin = csql_binary ();
    const std::string db = iset.database_name;
    const std::string user_s = (user != NULL && user[0] != '\0') ? user : "DBA";
    const std::string pw_s = (password != NULL) ? password : "";

    const std::string tbase = (getenv ("TMPDIR") != NULL) ? getenv ("TMPDIR") : "/tmp";
    std::string tmpls = tbase + "/importdb-validate-XXXXXX";
    std::vector<char> tmpl (tmpls.begin (), tmpls.end ());
    tmpl.push_back ('\0');
    if (mkdtemp (tmpl.data ()) == NULL)
      {
	return validate_fks (iset, graph, rebuild, continue_on_error, summary);
      }
    const std::string scratch (tmpl.data ());

    /* Concurrent count-only anti-joins, bounded by degree + the edge fan-out. */
    int deg = (degree < 1) ? 1 : degree;
    if ((size_t) deg > work.size ())
      {
	deg = (int) work.size ();
      }

    std::vector<std::string> logs (work.size ());
    std::map<pid_t, size_t> pid_idx;
    size_t next = 0;
    int running = 0;
    bool anomaly = false;

    while (next < work.size () || running > 0)
      {
	while (running < deg && next < work.size ())
	  {
	    logs[next] = scratch + "/count_" + std::to_string (next) + ".log";
	    pid_t pid = spawn_csql_count (bin, db, user_s, pw_s, build_antijoin_count (*work[next]), logs[next]);
	    if (pid < 0)
	      {
		anomaly = true;
	      }
	    else
	      {
		pid_idx[pid] = next;
		running++;
	      }
	    next++;
	  }
	if (running > 0)
	  {
	    int status = 0;
	    pid_t done = waitpid (-1, &status, 0);
	    if (done > 0 && pid_idx.count (done))
	      {
		pid_idx.erase (done);
		running--;
		if (! (WIFEXITED (status) && WEXITSTATUS (status) == 0))
		  {
		    anomaly = true;
		  }
	      }
	    else if (done < 0 && errno != EINTR)
	      {
		anomaly = true;
		break;
	      }
	  }
      }

    /* 0 = clean, >0 = violation, -1 = csql/parse error. */
    bool any_violation = false;
    if (!anomaly)
      {
	for (size_t i = 0; i < work.size (); i++)
	  {
	    int64_t c = parse_count (logs[i]);
	    if (c < 0)
	      {
		anomaly = true;
		break;
	      }
	    if (c > 0)
	      {
		any_violation = true;
	      }
	  }
      }

    for (const std::string &l : logs)
      {
	if (!l.empty ())
	  {
	    unlink (l.c_str ());
	  }
      }
    rmdir (scratch.c_str ());

    if (anomaly || any_violation)
      {
	/* Any violation, csql error, or lost child -> the verified serial path is
	 * authoritative: it re-runs the anti-joins in-process (seeing the committed
	 * data + rebuilt index), enumerates every offender into the exceptions
	 * artifact, and applies the fail-fast / --continue policy + diagnostics. */
	return validate_fks (iset, graph, rebuild, continue_on_error, summary);
      }

    /* All edges clean: build the summary in the SAME deterministic (child,
     * parent, name) order the serial path produces, so the manifest + report
     * are byte-identical to a serial clean run. */
    std::vector<const fk_edge *> all;
    for (const fk_edge &e : graph.fk_edges)
      {
	all.push_back (&e);
      }
    std::sort (all.begin (), all.end (), [] (const fk_edge *a, const fk_edge *b)
    {
      if (a->child != b->child)
	{
	  return a->child < b->child;
	}
      if (a->parent != b->parent)
	{
	  return a->parent < b->parent;
	}
      return a->name < b->name;
    });
    for (const fk_edge *e : all)
      {
	fk_edge_result r;
	r.child = e->child;
	r.parent = e->parent;
	r.name = e->name;
	if (withheld.count (e->name))
	  {
	    r.skipped = true;
	    summary.skipped_edges++;
	  }
	else
	  {
	    r.orphans = 0;
	    summary.validated_edges++;
	  }
	summary.edges.push_back (r);
      }
    fprintf (stdout, msg (IMPORTDB_MSG_VALIDATE_COMPLETE), graph.database_name.c_str (), summary.validated_edges,
	     summary.skipped_edges);
    return validate_status::OK;
  }

} // namespace cubimport
