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
 * import_define.cpp - Stage 2 (Definition phase) of the importdb pipeline
 *
 * Connects to the target DB in CS mode and executes the dump's definition DDL
 * to leave a fully-defined but empty target DB (10-design.md §2). For a DEFAULT
 * dump this is the single <prefix>_schema file (CREATE CLASS + columns + ADD
 * SUPERCLASS + serial + partition + PK/UK/FK); for a SPLIT dump it is the
 * schema-class file followed by the constraint files in the _schema_info apply
 * order (both already resolved by Discovery into iset.schema_apply_order). The
 * <prefix>_trigger and <prefix>_indexes artifacts are DEFERRED to terminal
 * WUs, and no object/data file is touched here.
 *
 * DDL execution reuses the SAME public primitive loaddb is built on -
 * db_make_session_for_one_statement_execution () plus the public
 * db_open_buffer / db_compile_statement / db_execute_statement loop
 * (modeled on the core of loaddb's ldr_exec_query_from_file, minus the
 * old-version client-type compat branches and loaddb-specific logging). No
 * loaddb static is exposed and no loaddb load_args is constructed. As loaddb
 * does for its schema file, authorization is disabled around execution for
 * DBA-group members. On a compile/execute error the offending file + line is
 * reported and the whole definition transaction is aborted (all-or-nothing).
 *
 * The connection is owned by the caller (import_session.hpp): define () assumes
 * the one CS-mode session is already open and shared with the Graph builder's
 * catalog snapshot (WU-21). It commits its DDL on success and aborts on error,
 * but never connects or shuts the session down.
 */

#include "import_define.hpp"

#include "db.h"
#include "authenticate.h"
#include "error_manager.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"
#include "porting.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace
{
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

  /*
   * Execute every SQL statement in one DDL file against the connected target.
   * Returns NO_ERROR on success, otherwise the failing statement's error code
   * (a diagnostic naming the file + line has already been emitted). Reuses the
   * public one-statement-at-a-time session primitive; the caller owns the
   * surrounding transaction (no commit/abort here). When commit is false (a
   * --dry-run) the in-file transaction-boundary statements (unloaddb schema
   * files end with an explicit COMMIT WORK) are skipped, so the whole
   * definition stays in one transaction the caller can abort intact.
   */
  int
  exec_ddl_file (const std::string &file_path, bool commit)
  {
    /* Read the file into a buffer and open a BUFFER session, the way csql does for
     * its own file input (csql.c:2218 db_open_buffer). The file-stream primitive
     * db_make_session_for_one_statement_execution () was the obvious reuse of
     * loaddb's shape, but it costs replication: parser->original_buffer is set only
     * on the string-parsing path (parse_tree_cl.c:1983), sql_user_text is copied
     * from it (parse_tree_cl.c:2456), and do_replicate_statement () returns early
     * when sql_user_text is empty -- with the comment "this should be loaddb"
     * (execute_statement.c:16868). Under a file session every CREATE CLASS here is
     * therefore skipped for HA replication while the later ALTER CLASS statements
     * of strip/rebuild/fkdefine, which run through db_execute () on a string, are
     * replicated. The slave then receives ALTER for classes it was never told to
     * create. Measured on a two-node pair: 22 apply failures, 0 rows, no user
     * classes (feature_tests/importdb/m5/ha51b-docker/findings/).
     *
     * The statement semantics are unchanged -- same compile/execute loop, same
     * order, same transaction ownership. What changes is that the whole file is
     * parsed at open rather than one statement at a time, so a syntax error
     * anywhere surfaces here instead of when execution reaches it. Schema files are
     * small (the object data is a different file entirely), so holding one in
     * memory is bounded. */
    std::string buf;
    {
      FILE *fp = fopen (file_path.c_str (), "r");
      if (fp == NULL)
	{
	  PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_DEFINE_FILE_OPEN_FAILED), file_path.c_str (), strerror (errno));
	  return ER_GENERIC_ERROR;
	}
      char chunk[8192];
      size_t n;
      while ((n = fread (chunk, 1, sizeof (chunk), fp)) > 0)
	{
	  buf.append (chunk, n);
	}
      fclose (fp);
    }

    DB_SESSION *session = db_open_buffer (buf.c_str ());
    if (session == NULL || db_get_errors (session) != NULL)
      {
	int line = 0, col = 0;
	if (session != NULL)
	  {
	    db_get_parser_line_col (session, &line, &col);
	  }
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_DEFINE_STMT_FAILED), file_path.c_str (), line, db_error_string (3));
	if (session != NULL)
	  {
	    db_close_session (session);
	  }
	return (er_errid () != NO_ERROR) ? er_errid () : ER_GENERIC_ERROR;
      }

    int error = NO_ERROR;
    while (true)
      {
	/* A buffer session has already parsed every statement; db_compile_statement
	 * walks them in order and returns 0 at the end (csql.c uses the same loop). */
	int stmt_id = db_compile_statement (session);
	int stmt_cnt = stmt_id;

	/* stmt_cnt <= 0 is either clean EOF (no session error) or a parse
	 * error; stmt_id <= 0 is a compile error. Report any and stop. */
	if (stmt_cnt <= 0 || stmt_id <= 0)
	  {
	    DB_SESSION_ERROR *session_error = db_get_errors (session);
	    if (session_error != NULL)
	      {
		int line, col;
		do
		  {
		    session_error = db_get_next_error (session_error, &line, &col);
		    if (line <= 0)
		      {
			db_get_parser_line_col (session, &line, &col);
		      }
		    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_DEFINE_STMT_FAILED), file_path.c_str (), line,
					   db_error_string (3));
		    assert (er_errid () != NO_ERROR);
		    error = er_errid ();
		  }
		while (session_error);
	      }
	    db_close_session (session);
	    break;
	  }

	/* On a --dry-run the definition must stay in ONE abortable transaction,
	 * but unloaddb schema files end with an explicit COMMIT WORK (and could
	 * carry other transaction-boundary statements). Skip executing those so
	 * nothing is committed mid-define; the caller's session_close(false)
	 * then rolls the entire definition back and the target is untouched. */
	if (!commit)
	  {
	    int stmt_type = db_get_statement_type (session, stmt_id);
	    if (stmt_type == CUBRID_STMT_COMMIT_WORK || stmt_type == CUBRID_STMT_ROLLBACK_WORK)
	      {
		continue;
	      }
	  }

	DB_QUERY_RESULT *res = NULL;
	error = db_execute_statement (session, stmt_id, &res);
	if (error < 0)
	  {
	    int line, col;
	    db_get_parser_line_col (session, &line, &col);
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_DEFINE_STMT_FAILED), file_path.c_str (), line, db_error_string (3));
	    db_close_session (session);
	    break;
	  }
	error = db_query_end (res);
	if (error < 0)
	  {
	    int line, col;
	    db_get_parser_line_col (session, &line, &col);
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_DEFINE_STMT_FAILED), file_path.c_str (), line, db_error_string (3));
	    db_close_session (session);
	    break;
	  }
      }

    return error;
  }

  /*
   * Execute the definition DDL for iset in the connected target. DEFAULT runs
   * the single schema file; SPLIT runs the schema-class file then the
   * constraint files, both already in apply order in schema_apply_order. commit
   * is threaded to exec_ddl_file so a --dry-run keeps the whole definition in
   * one abortable transaction.
   */
  int
  run_definition_ddl (const cubimport::import_set &iset, bool commit)
  {
    if (iset.schema_kind == cubimport::schema_layout::SPLIT)
      {
	for (const std::string &f : iset.schema_apply_order)
	  {
	    int error = exec_ddl_file (path_join (iset.dump_dir, f), commit);
	    if (error != NO_ERROR)
	      {
		return error;
	      }
	  }
	return NO_ERROR;
      }

    return exec_ddl_file (path_join (iset.dump_dir, iset.schema_file), commit);
  }
} // namespace

namespace cubimport
{

  define_status
  define (const import_set &iset, bool commit)
  {
    /* This is the definition phase only: trigger actions must not fire. */
    db_disable_trigger ();

    /* As ldr_load_schema_file does, DBA-group members run the definition DDL
     * with authorization disabled (compat with pre-authorization dumps). */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);
    int error = run_definition_ddl (iset, commit);
    AU_RESTORE (au_save);

    if (error != NO_ERROR)
      {
	db_abort_transaction ();
	return define_status::ERR_DDL;
      }

    /* On a normal run commit so the target is durably defined and empty. On a
     * --dry-run (commit=false) leave the DDL uncommitted in the open
     * transaction: the Graph builder's snapshot still sees it on this session,
     * and the caller's session_close(false) aborts it so the target is left
     * completely unchanged (CUBRID DDL is transactional). */
    if (commit)
      {
	db_commit_transaction ();
      }
    fprintf (stdout, msg (IMPORTDB_MSG_DEFINE_COMPLETE), iset.database_name.c_str ());
    return define_status::OK;
  }

} // namespace cubimport
