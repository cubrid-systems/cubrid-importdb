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
 * import_triggers.cpp - Stage 4 (Trigger define, last terminal task of WU-35)
 *
 * Defines the dump's deferred triggers (<prefix>_trigger) STRICTLY LAST - after
 * the data is loaded and every constraint + statistic is in place - so the
 * triggers exist for future DML only and never re-fire on the imported rows (the
 * unloaddb dump already holds the post-trigger data; that is why the Definition
 * phase WU-20 held the trigger file back). It is the terminal task of the
 * importdb pipeline (10-design.md §5/§6).
 *
 * The file is executed through the SAME public one-statement-at-a-time primitive
 * the Definition phase uses (db_make_session_for_one_statement_execution + the
 * db_parse_one_statement / db_compile_statement / db_execute_statement loop),
 * NOT the Rebuild phase's semicolon split: a CREATE TRIGGER action body can carry
 * embedded semicolons and string literals, so only the parser may delimit the
 * statements. Authorization is disabled for the DBA-group path (as the Definition
 * phase does); an embedded transaction-boundary statement (unloaddb files end
 * with COMMIT WORK) is skipped so the phase never commits mid-run.
 *
 * Triggers are defined only after the imported data + constraints are complete,
 * so a CREATE TRIGGER failure must not roll the import back: on an execute failure
 * the phase records it (summary.failed), emits a diagnostic naming the file +
 * line, and CONTINUES to the next statement (a CUBRID statement failure does not
 * abort the transaction). A parse/compile error - not expected from a machine-
 * generated trigger file - is recorded and stops the file (the parser cannot
 * reliably resynchronize). When any statement failed define_triggers () returns
 * PARTIAL and the run exits non-zero while the imported result + the successfully-
 * defined triggers still commit. The caller owns the transaction
 * (import_session.hpp): define_triggers () neither commits nor aborts.
 */

#include "import_triggers.hpp"
#include "import_progress.hpp"

#include "db.h"
#include "authenticate.h"
#include "error_manager.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <cerrno>
#include <cstring>
#include <string>

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
} // namespace

namespace cubimport
{

  trigger_status
  define_triggers (const import_set &iset, trigger_summary &summary, bool resume)
  {
    summary = trigger_summary ();
    summary.trigger_file = iset.trigger_file;

    /* A dump with no trigger file is nothing to do. */
    if (iset.trigger_file.empty ())
      {
	return trigger_status::OK;
      }

    const std::string file_path = path_join (iset.dump_dir, iset.trigger_file);
    FILE *fp = fopen (file_path.c_str (), "r");
    if (fp == NULL)
      {
	IMPORT_ERR (msg (IMPORTDB_MSG_TRIGGER_FILE_OPEN_FAILED), file_path.c_str (), strerror (errno));
	summary.failed++;
	return trigger_status::PARTIAL;
      }

    /* As the Definition phase does, DBA-group members run the trigger DDL with
     * authorization disabled. */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    DB_SESSION *session = db_make_session_for_one_statement_execution (fp);
    if (session == NULL)
      {
	assert (er_errid () != NO_ERROR);
	IMPORT_ERR (msg (IMPORTDB_MSG_TRIGGER_STMT_FAILED), file_path.c_str (), 0, db_error_string (3));
	AU_RESTORE (au_save);
	fclose (fp);
	summary.failed++;
	return trigger_status::PARTIAL;
      }

    while (true)
      {
	int stmt_id = 0;
	int stmt_cnt = db_parse_one_statement (session);
	if (stmt_cnt > 0)
	  {
	    stmt_id = db_compile_statement (session);
	  }

	/* stmt_cnt <= 0 is either clean EOF (no session error) or a parse
	 * error; stmt_id <= 0 is a compile error. A machine-generated trigger
	 * file should hit neither except at EOF; report a real error and stop
	 * (the parser cannot reliably resynchronize past a syntax error). */
	if (stmt_cnt <= 0 || stmt_id <= 0)
	  {
	    DB_SESSION_ERROR *session_error = db_get_errors (session);
	    if (session_error != NULL)
	      {
		int line, col;
		/* One statement failed to parse/compile - count it ONCE, however many
		 * chained error records the session attached (the do/while below only
		 * drives the per-record diagnostics, not the failure tally). */
		summary.failed++;
		do
		  {
		    session_error = db_get_next_error (session_error, &line, &col);
		    if (line <= 0)
		      {
			db_get_parser_line_col (session, &line, &col);
		      }
		    IMPORT_ERR (msg (IMPORTDB_MSG_TRIGGER_STMT_FAILED), file_path.c_str (), line,
					   db_error_string (3));
		  }
		while (session_error);
	      }
	    break;
	  }

	/* Skip embedded transaction-boundary statements (unloaddb trigger files
	 * end with COMMIT WORK) so the phase does not commit mid-run - the caller
	 * owns the final commit. */
	int stmt_type = db_get_statement_type (session, stmt_id);
	if (stmt_type == CUBRID_STMT_COMMIT_WORK || stmt_type == CUBRID_STMT_ROLLBACK_WORK)
	  {
	    continue;
	  }

	DB_QUERY_RESULT *res = NULL;
	int error = db_execute_statement (session, stmt_id, &res);
	if (error < 0)
	  {
	    /* WU-50 resume: the interrupted run already defined this trigger.
	     * That is completed work, not a failure. */
	    if (resume && er_errid () == ER_TR_TRIGGER_EXISTS)
	      {
		summary.defined++;
		continue;
	      }

	    /* Non-fatal: record the failing statement + line and continue. A
	     * CUBRID statement failure does not abort the transaction, so the
	     * next trigger statement can still run. */
	    int line, col;
	    db_get_parser_line_col (session, &line, &col);
	    IMPORT_ERR (msg (IMPORTDB_MSG_TRIGGER_STMT_FAILED), file_path.c_str (), line, db_error_string (3));
	    summary.failed++;
	    continue;
	  }
	db_query_end (res);
	summary.defined++;
	cubimport::progress::set_detail ("defined " + std::to_string (summary.defined) + " trigger(s)");
      }

    db_close_session (session);
    AU_RESTORE (au_save);
    fclose (fp);

    if (summary.failed > 0)
      {
	IMPORT_PRINT (msg (IMPORTDB_MSG_TRIGGER_PARTIAL), summary.defined, summary.failed,
		 iset.trigger_file.c_str ());
	return trigger_status::PARTIAL;
      }

    IMPORT_PRINT (msg (IMPORTDB_MSG_TRIGGER_COMPLETE), summary.defined, iset.trigger_file.c_str ());
    return trigger_status::OK;
  }

} // namespace cubimport
