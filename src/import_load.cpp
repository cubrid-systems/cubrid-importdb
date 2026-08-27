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
 * import_load.cpp - Stage 4 (Load phase, step 2 of the constraint lifecycle)
 *
 * Pours the ImportSet's object data into the stripped (bare-heap) target by
 * REUSING the loaddb data-load path through its PUBLIC client stubs only -
 * loaddb source is untouched and no loaddb static is exposed. The flow mirrors
 * load_db.c's ldr_server_load () + load_object_file (), per object file:
 *   loaddb_init (load_args)  -> set up the server load session + worker pool,
 *   cubload::split ()        -> parse the file into batches, driving the
 *     class_handler (loaddb_install_class) and batch_handler (loaddb_load_batch),
 *   poll loaddb_fetch_status () until the session reports completed/failed,
 *   loaddb_destroy ()        -> tear the session down.
 * The loaddb-only concerns are dropped: no logddl, no -S / SA path, no
 * estimated-size, no compare-storage-order, and NO statistics update (statistics
 * are rebuilt in a later WU). The load_args we construct set only what a
 * value-only CS-mode object load into bare heaps needs: the object file path,
 * the batch/commit size (loaddb's default), disable-statistics, the effective
 * user (for the bare-name %class fallback), and the skipped-class ignore list.
 *
 * Serial across tables (inter-table parallelism is M4/WU-40); each object file
 * gets its own loaddb session, which matches loaddb's one-file-per-session model
 * and keeps split ()'s per-file class-id numbering independent across files. The
 * load workers commit their own batches server-side, so the loaded rows are
 * durable independently of the caller's transaction; the caller owns only the
 * schema transaction (import_session.hpp) and commits it at the end.
 */

#include "import_load.hpp"

#include "network_interface_cl.h"	/* loaddb_* public stubs + cubload::load_args/batch/split */

#include "db.h"
#include "dbtype.h"			/* db_get_bigint / db_value_clear - post-load row counts */
#include "error_manager.h"
#include "environment_variable.h"	/* envvar_bindir_file - locate the cub_admin binary */

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"
#include "porting.h"

#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  /* ASCII lowercase copy (skipped-class names are compared against loaddb's
   * lowercased object-file class identifiers). */
  std::string
  to_lower (const std::string &s)
  {
    std::string out (s);
    for (char &c : out)
      {
	c = (char) std::tolower ((unsigned char) c);
      }
    return out;
  }

  /* Absolute form of dir (cubload::split requires an absolute object-file path).
   * Falls back to dir unchanged if it cannot be resolved. */
  std::string
  absolute_dir (const std::string &dir)
  {
    char resolved[PATH_MAX];
    if (realpath (dir.c_str (), resolved) != NULL)
      {
	return std::string (resolved);
      }
    return dir;
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

  /*
   * For a PER_CLASS object file "<prefix>_<owner>.<class>_objects", return the
   * bare class name lowercased; returns an empty string for a SINGLE-layout
   * "<prefix>_objects" (which carries every class and cannot be skipped as a
   * whole file). Used to skip a whole per-class file whose class was excluded
   * by --skip-object-classes.
   */
  std::string
  object_file_class (const std::string &prefix, const std::string &basename)
  {
    const std::string lead = prefix + "_";
    const std::string suffix = "_objects";
    if (basename.size () <= lead.size () + suffix.size ())
      {
	return "";
      }
    if (basename.compare (0, lead.size (), lead) != 0
	|| basename.compare (basename.size () - suffix.size (), suffix.size (), suffix) != 0)
      {
	return "";
      }
    std::string mid = basename.substr (lead.size (), basename.size () - lead.size () - suffix.size ());
    if (mid.empty ())
      {
	return "";
      }
    size_t dot = mid.find_last_of ('.');
    return to_lower (dot == std::string::npos ? mid : mid.substr (dot + 1));
  }

  /*
   * Build the load_args for one object file. Only the fields a value-only
   * CS-mode object load into bare heaps needs are set; everything else keeps the
   * load_args () defaults. periodic_commit is loaddb's default batch/commit size
   * (also the split () batch size); disable_statistics is set because statistics
   * are rebuilt in a later WU (we never call loaddb_update_stats here);
   * user_name is only consulted by the server for a bare (non-owner-qualified)
   * %class name; ignore_classes carries the skipped (object-valued) classes.
   */
  void
  build_load_args (cubload::load_args &args, const std::string &object_file_abs, const std::string &user,
		   const std::vector<std::string> &ignore)
  {
    args.object_file = object_file_abs;
    args.periodic_commit = cubload::load_args::PERIODIC_COMMIT_DEFAULT_VALUE;
    args.verbose_commit = false;
    args.disable_statistics = true;
    args.user_name = user;
    args.ignore_classes = ignore;
  }

  /*
   * Load one object file in its own loaddb session (mirrors ldr_server_load () +
   * load_object_file ()). On success sets rows/failed to the session totals and
   * returns NO_ERROR; on failure sets err to the surfaced error text and returns
   * a non-NO_ERROR code. The load workers commit their batches server-side.
   */
  int
  load_one_file (const cubload::load_args &args_in, int64_t &rows, int64_t &failed, std::string &err)
  {
    cubload::load_args args = args_in;	/* loaddb_init takes a mutable reference */

    int error = loaddb_init (args);
    if (error != NO_ERROR)
      {
	err = db_error_string (3);
	return error;
      }

    std::string load_err;

    /* *INDENT-OFF* */
    cubload::batch_handler b_handler = [&] (const cubload::batch &b) -> int
    {
      int rc = NO_ERROR;
      bool use_temp_batch = false;
      bool is_batch_accepted = false;
      do
	{
	  cubload::load_status status;
	  rc = loaddb_load_batch (b, use_temp_batch, is_batch_accepted, status);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }
	  use_temp_batch = true;	/* do not re-upload the batch while retrying */
	  for (const cubload::stats &s : status.get_load_stats ())
	    {
	      if (load_err.empty () && !s.error_message.empty ())
		{
		  load_err = s.error_message;
		}
	    }
	}
      while (!is_batch_accepted);
      return rc;
    };

    cubload::class_handler c_handler = [] (const cubload::batch &b, bool &is_ignored) -> int
    {
      std::string class_name;
      return loaddb_install_class (b, is_ignored, class_name);
    };
    /* *INDENT-ON* */

    error = cubload::split (args.periodic_commit, args.object_file, c_handler, b_handler);
    if (error != NO_ERROR)
      {
	loaddb_interrupt ();
      }

    /* Drain: wait until the load session reports completion (all submitted
     * batches processed) or failure, collecting the running totals. */
    cubload::load_status status;
    do
      {
	int fetch_error = loaddb_fetch_status (status);
	if (fetch_error != NO_ERROR)
	  {
	    loaddb_interrupt ();
	    if (error == NO_ERROR)
	      {
		error = fetch_error;
	      }
	    break;
	  }
	if (!status.get_load_stats ().empty ())
	  {
	    const cubload::stats &last = status.get_load_stats ().back ();
	    rows = last.rows_committed;
	    failed = last.rows_failed;
	    if (load_err.empty () && !last.error_message.empty ())
	      {
		load_err = last.error_message;
	      }
	  }
	std::this_thread::sleep_for (std::chrono::milliseconds (100));
      }
    while (! (status.is_load_completed () || status.is_load_failed ()));

    bool session_failed = (error != NO_ERROR) || status.is_load_failed () || !load_err.empty ();

    int destroy_error = loaddb_destroy ();

    if (session_failed)
      {
	err = !load_err.empty () ? load_err : std::string (db_error_string (3));
	return (error != NO_ERROR) ? error : ER_FAILED;
      }
    if (destroy_error != NO_ERROR)
      {
	err = db_error_string (3);
	return destroy_error;
      }
    return NO_ERROR;
  }

  /* ---- WU-40 inter-table parallel load: bounded `cub_admin loaddb -C` pool ---- */

  /* The admin-utility binary path ($CUBRID/bin/cub_admin). Falls back to
   * "cub_admin" (PATH lookup via execvp) if the install dir cannot be resolved.
   *
   * We exec cub_admin DIRECTLY rather than going through `cubrid loaddb`, and the
   * reason is process-tree depth, not speed. `cubrid` is the util_service driver:
   * process_admin () copies argv verbatim and fork/execv's cub_admin, then
   * waitpid ()s for it (util_service.c:465-480, :888-908), and cub_admin takes the
   * utility name as argv[1] (util_admin.c:1134). So `cubrid loaddb <args>` and
   * `cub_admin loaddb <args>` reach the same loaddb entry point with the same
   * argv - the driver adds a process, not behaviour.
   *
   * That extra process is what made a killed import leak loaders. With the driver
   * in the middle the real loader is our GRANDCHILD: kill importdb and the driver
   * is orphaned (blocked in waitpid, nothing wakes it) while the loader's parent -
   * the driver - is still alive, so the loader's own getppid () never changes and
   * it keeps writing into a database a resumed run is about to truncate. Collapsing
   * the level makes the loader a direct child, which is what lets PR_SET_PDEATHSIG
   * below actually reach it.
   *
   * The one thing the driver did that we lose is hide_cmd_line_args () masking its
   * own argv after the fork (util_service.c:918). That never covered the loader:
   * cub_admin keeps the real `-p <pw>` on its command line either way (see the
   * password note in spawn_loaddb below), so the exposure is unchanged - one
   * redundant copy of an already-visible secret goes away with the process. */
  std::string
  cub_admin_binary ()
  {
    char path[PATH_MAX];
    if (envvar_bindir_file (path, sizeof (path), "cub_admin") != NULL)
      {
	return std::string (path);
      }
    return "cub_admin";
  }

  /* The last non-blank line of a child's captured log (surfaced in the failure
   * diagnostic); empty when the log is missing/empty. */
  std::string
  last_log_line (const std::string &path)
  {
    FILE *fp = fopen (path.c_str (), "r");
    if (fp == NULL)
      {
	return "";
      }
    char buf[1024];
    std::string last;
    while (fgets (buf, sizeof (buf), fp) != NULL)
      {
	std::string line (buf);
	while (!line.empty () && (line.back () == '\n' || line.back () == '\r'))
	  {
	    line.pop_back ();
	  }
	if (!line.empty ())
	  {
	    last = line;
	  }
      }
    fclose (fp);
    return last;
  }

  /*
   * Spawn one `cub_admin loaddb -C -u <user> [-p <pw>] --no-statistics
   * [--ignore-class-file <f>] -d <object-file> <db>` child, its stdout+stderr
   * redirected to log_path. Returns the child pid, or -1 if fork failed.
   */
  pid_t
  spawn_loaddb (const std::string &bin, const std::string &db, const std::string &user, const std::string &password,
		const std::string &object_file, const std::string &ignore_file, const std::string &log_path)
  {
    /* Build argv (and cache the C strings) in the PARENT: everything the child
     * does between fork () and execvp () must be async-signal-safe, so no heap
     * allocation / std::string / std::vector work may happen post-fork (it can
     * deadlock if any other client thread holds the malloc lock at fork time). */
    std::vector<std::string> a = { "cub_admin", "loaddb", "-C", "-u", user, "--no-statistics" };
    /* The password rides on the child's command line (-p), so it is briefly
     * visible in `ps` / /proc/<pid>/cmdline for the child's lifetime. This is the
     * standard CUBRID CS-tool convention: loaddb/csql accept a batch password
     * only via -p (their interactive getpass prompt needs a tty, which a forked
     * -C child does not have), and there is no env/stdin channel to substitute. */
    if (!password.empty ())
      {
	a.push_back ("-p");
	a.push_back (password);
      }
    if (!ignore_file.empty ())
      {
	a.push_back ("--ignore-class-file");
	a.push_back (ignore_file);
      }
    a.push_back ("-d");
    a.push_back (object_file);
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

    /* Read in the PARENT, for the child's post-prctl orphan re-check below. */
    const pid_t parent_pid = getpid ();

    pid_t pid = fork ();
    if (pid < 0)
      {
	return -1;
      }
    if (pid == 0)
      {
	/* child: async-signal-safe calls only (close/open/dup2/execvp) - argv +
	 * the C strings were built by the parent above and inherited copy-on-write.
	 * Close every inherited descriptor above stderr FIRST: the parent is a CS
	 * client holding an open server-connection socket that is not FD_CLOEXEC, so
	 * without this the exec'd loaddb child would keep a copy of that socket open
	 * for its whole lifetime (mirrors dynamic_load.c's post-fork child). */
	for (int i = STDERR_FILENO + 1; i < (int) open_max; ++i)
	  {
	    close (i);
	  }
	/* Die with the parent. A load child outlives an importdb that was killed
	 * without its process group (a bare `kill <pid>`, a harness that only knows
	 * the top pid, an OOM kill) and keeps committing rows into a target that a
	 * resumed run then truncates and reloads - the row count comes out doubled.
	 * PR_SET_PDEATHSIG survives execve and is delivered by the kernel, so it
	 * covers the case a cleanup handler cannot: SIGKILL of importdb itself, where
	 * nothing of ours gets to run. It reaches this process only because we exec
	 * cub_admin directly (see cub_admin_binary ()) - it fires on the death of the
	 * IMMEDIATE parent, so with the `cubrid` driver in between it would have
	 * killed the driver and left the loader.
	 *
	 * The classic race: if the parent died between fork () and here, the signal
	 * was already sent to nobody and would never arrive. Re-read getppid () - a
	 * parent that changed means we are already orphaned, so leave now. Both calls
	 * are async-signal-safe, which the post-fork child requires. */
	prctl (PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
	if (getppid () != parent_pid)
	  {
	    _exit (128 + SIGKILL);
	  }
	int fd = open (log_c, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0)
	  {
	    dup2 (fd, STDOUT_FILENO);
	    dup2 (fd, STDERR_FILENO);
	    close (fd);
	  }
	execvp (bin_c, argv.data ());
	_exit (127);		/* exec failed */
      }
    return pid;
  }

  /* Post-load row count of one class via the caller's connection (the children
   * committed their data). Returns -1 on query error. */
  int64_t
  class_row_count (const std::string &cls)
  {
    std::string q = "SELECT count(*) FROM [" + cls + "]";
    DB_QUERY_RESULT *result = NULL;
    DB_QUERY_ERROR query_error;
    if (db_execute (q.c_str (), &result, &query_error) < 0)
      {
	return -1;
      }
    int64_t n = -1;
    if (db_query_first_tuple (result) == DB_CURSOR_SUCCESS)
      {
	DB_VALUE v;
	if (db_query_get_tuple_value (result, 0, &v) == NO_ERROR)
	  {
	    n = db_get_bigint (&v);
	    db_value_clear (&v);
	  }
      }
    db_query_end (result);
    return n;
  }
} // namespace

namespace cubimport
{

  load_data_status
  load_data (const import_set &iset, const dependency_graph &graph, load_summary &summary)
  {
    const std::string dump_dir_abs = absolute_dir (iset.dump_dir);

    /* Skipped (object-valued) classes are excluded from graph.nodes and listed
     * in graph.skipped_classes; they must not be loaded. Feed loaddb's native
     * ignore mechanism (SINGLE: the one file carries every class), and, for a
     * PER_CLASS file, skip the whole file whose class was excluded. */
    std::vector<std::string> ignore_classes;
    for (const std::string &c : graph.skipped_classes)
      {
	ignore_classes.push_back (to_lower (c));
      }

    /* The effective user is only consulted for a bare (non-owner-qualified)
     * %class name; unloaddb 11.2+ dumps are owner-qualified. */
    const char *user = db_get_user_name ();
    const std::string user_name = (user != NULL) ? user : "";

    for (const std::string &object_file : iset.object_files)
      {
	if (!ignore_classes.empty ())
	  {
	    const std::string cls = object_file_class (iset.prefix, object_file);
	    if (!cls.empty ()
		&& std::find (ignore_classes.begin (), ignore_classes.end (), cls) != ignore_classes.end ())
	      {
		continue;	/* per-class file for an excluded class */
	      }
	  }

	cubload::load_args args;
	build_load_args (args, path_join (dump_dir_abs, object_file), user_name, ignore_classes);

	int64_t rows = 0;
	int64_t failed = 0;
	std::string err;
	if (load_one_file (args, rows, failed, err) != NO_ERROR)
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_LOAD_FAILED), object_file.c_str (), err.c_str ());
	    return load_data_status::ERR_LOAD;
	  }

	summary.files.push_back ({ object_file, rows, failed });
	summary.total_rows += rows;
	summary.total_failed += failed;
	summary.loaded_files++;
      }

    fprintf (stdout, msg (IMPORTDB_MSG_LOAD_COMPLETE), (long) summary.total_rows, summary.loaded_files,
	     iset.database_name.c_str ());
    return load_data_status::OK;
  }

  load_data_status
  load_data_parallel (const import_set &iset, const dependency_graph &graph, load_summary &summary, int degree,
		      const char *user, const char *password)
  {
    const std::string dump_dir_abs = absolute_dir (iset.dump_dir);
    const std::string bin = cub_admin_binary ();
    const std::string db = iset.database_name;
    const std::string user_s = (user != NULL && user[0] != '\0') ? user : "DBA";
    const std::string pw_s = (password != NULL) ? password : "";

    /* skipped (object-valued) classes, lowercased for object-file matching */
    std::vector<std::string> ignore_classes;
    for (const std::string &c : graph.skipped_classes)
      {
	ignore_classes.push_back (to_lower (c));
      }

    /* the object files to spawn a load for: a PER_CLASS file whose class was
     * excluded is not spawned; a SINGLE file carrying skipped classes is loaded
     * with a --ignore-class-file (built below). */
    std::vector<std::string> files;
    for (const std::string &object_file : iset.object_files)
      {
	if (!ignore_classes.empty ())
	  {
	    const std::string cls = object_file_class (iset.prefix, object_file);
	    if (!cls.empty ()
		&& std::find (ignore_classes.begin (), ignore_classes.end (), cls) != ignore_classes.end ())
	      {
		continue;
	      }
	  }
	files.push_back (object_file);
      }

    /* scratch dir for the ignore-class file + per-child logs */
    const std::string tbase = (getenv ("TMPDIR") != NULL) ? getenv ("TMPDIR") : "/tmp";
    std::string tmpls = tbase + "/importdb-load-XXXXXX";
    std::vector<char> tmpl (tmpls.begin (), tmpls.end ());
    tmpl.push_back ('\0');
    if (mkdtemp (tmpl.data ()) == NULL)
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_LOAD_FAILED), "(parallel load setup)",
			       "cannot create a temporary directory for the parallel load");
	return load_data_status::ERR_LOAD;
      }
    const std::string scratch (tmpl.data ());

    /* SINGLE layout carrying skipped classes -> loaddb --ignore-class-file */
    std::string ignore_file;
    if (!ignore_classes.empty () && iset.object_kind == object_layout::SINGLE)
      {
	ignore_file = scratch + "/ignore_classes";
	FILE *f = fopen (ignore_file.c_str (), "w");
	if (f == NULL)
	  {
	    /* fail fast rather than hand the children a missing --ignore-class-file */
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_LOAD_FAILED), ignore_file.c_str (), strerror (errno));
	    rmdir (scratch.c_str ());
	    return load_data_status::ERR_LOAD;
	  }
	for (const std::string &c : ignore_classes)
	  {
	    fprintf (f, "%s\n", c.c_str ());
	  }
	fclose (f);
      }

    /* bounded pool: at most `degree` concurrent children, also bounded by the
     * object-file fan-out (cannot parallelize beyond the number of files). */
    int deg = (degree < 1) ? 1 : degree;
    if (!files.empty () && (size_t) deg > files.size ())
      {
	deg = (int) files.size ();
      }

    std::map<pid_t, size_t> pid_idx;
    std::vector<std::string> logs (files.size ());
    bool any_fail = false;
    std::string fail_file, fail_msg;
    size_t next = 0;
    int running = 0;

    while (next < files.size () || running > 0)
      {
	while (running < deg && next < files.size ())
	  {
	    logs[next] = scratch + "/load_" + std::to_string (next) + ".log";
	    pid_t pid = spawn_loaddb (bin, db, user_s, pw_s, path_join (dump_dir_abs, files[next]), ignore_file,
				      logs[next]);
	    if (pid < 0)
	      {
		if (!any_fail)
		  {
		    any_fail = true;
		    fail_file = files[next];
		    fail_msg = "fork failed";
		  }
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
		/* only account for OUR tracked children (guards against reaping an
		 * unrelated pid and exiting the pool early / mis-blaming files[0]). */
		size_t idx = pid_idx[done];
		pid_idx.erase (done);
		running--;
		int ec = WIFEXITED (status) ? WEXITSTATUS (status) : -1;
		if (ec != 0 && !any_fail)
		  {
		    any_fail = true;
		    fail_file = files[idx];
		    std::string tail = last_log_line (logs[idx]);
		    fail_msg = tail.empty () ? ("loaddb -C exited with status " + std::to_string (ec)) : tail;
		  }
	      }
	    else if (done < 0 && errno != EINTR)
	      {
		/* ECHILD (no children left to reap) or another hard waitpid error:
		 * stop rather than spin forever. The still-"running" children are
		 * unaccounted for -> treat as a failure so the run never silently
		 * under-reports a lost load. */
		if (!any_fail)
		  {
		    any_fail = true;
		    fail_file = pid_idx.empty () ? std::string ("<unknown>") : files[pid_idx.begin ()->second];
		    fail_msg = "lost track of a load child process (waitpid failed)";
		  }
		break;
	      }
	  }
      }

    if (any_fail)
      {
	/* a child failed: name the object file + the child's last log line, and
	 * leave the scratch logs in place for the operator to inspect. */
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_LOAD_FAILED), fail_file.c_str (), fail_msg.c_str ());
	return load_data_status::ERR_LOAD;
      }

    /* Row counts are CATALOG-DERIVED: the children committed their data on their
     * own connections, so end the caller's (idle) transaction to get a fresh
     * snapshot, then SELECT count(*) per class. A child that hit row failures
     * would have exited non-zero and been caught above, so the per-file `failed`
     * is 0 here by construction. A count-query failure is surfaced (not silently
     * reported as 0 rows) but does not fail the run - the data is committed. */
    db_commit_transaction ();
    if (iset.object_kind == object_layout::PER_CLASS)
      {
	for (const std::string &object_file : files)
	  {
	    const std::string cls = object_file_class (iset.prefix, object_file);
	    int64_t n = cls.empty () ? 0 : class_row_count (cls);
	    if (n < 0)
	      {
		fprintf (stderr, "importdb: warning: post-load row count for '%s' unavailable; "
			 "manifest row totals may be incomplete\n", cls.c_str ());
		n = 0;
	      }
	    summary.files.push_back ({ object_file, n, 0 });
	    summary.total_rows += n;
	    summary.loaded_files++;
	  }
      }
    else
      {
	int64_t total = 0;
	for (const graph_node &nd : graph.nodes)
	  {
	    int64_t c = class_row_count (nd.name);
	    if (c < 0)
	      {
		fprintf (stderr, "importdb: warning: post-load row count for '%s' unavailable; "
			 "manifest row totals may be incomplete\n", nd.name.c_str ());
		continue;
	      }
	    total += c;
	  }
	summary.total_rows = total;
	summary.loaded_files = (int) files.size ();
	if (!files.empty ())
	  {
	    summary.files.push_back ({ files[0], total, 0 });
	  }
      }

    /* success: clean up the scratch logs + ignore file. */
    for (const std::string &l : logs)
      {
	if (!l.empty ())
	  {
	    unlink (l.c_str ());
	  }
      }
    if (!ignore_file.empty ())
      {
	unlink (ignore_file.c_str ());
      }
    rmdir (scratch.c_str ());

    fprintf (stdout, msg (IMPORTDB_MSG_LOAD_PARALLEL_COMPLETE), (long) summary.total_rows, summary.loaded_files,
	     iset.database_name.c_str (), deg);
    return load_data_status::OK;
  }

} // namespace cubimport
