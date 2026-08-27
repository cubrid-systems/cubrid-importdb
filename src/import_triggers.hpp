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
 * import_triggers.hpp - Stage 4 (Trigger define, last terminal task of WU-35)
 *
 * The Definition phase (WU-20) deferred the dump's <prefix>_trigger artifact so
 * no trigger would fire on the bulk-loaded rows (the unloaddb dump already holds
 * the post-trigger data). This phase defines those triggers, STRICTLY LAST -
 * after the data is loaded and every constraint (PK/UK/FK/index) and the
 * statistics are in place - so the triggers exist for future DML only, never
 * re-firing on the imported rows. It is the terminal task of the importdb
 * pipeline (10-design.md §5/§6: DEFINE_TRIGGERS is scheduled after every other
 * terminal task).
 *
 * The trigger file is executed as a whole DDL file through the SAME public
 * one-statement-at-a-time primitive the Definition phase uses
 * (db_make_session_for_one_statement_execution + the db_parse_one_statement /
 * db_compile_statement / db_execute_statement loop), NOT the semicolon-split path
 * the Rebuild phase uses: a CREATE TRIGGER action body can carry embedded
 * semicolons and string literals, so only the parser may delimit the statements.
 * Authorization is disabled for the DBA-group path, as the Definition phase does.
 * An embedded transaction-boundary statement (unloaddb files end with COMMIT
 * WORK) is skipped so the phase does not commit mid-run - the caller owns the
 * final commit.
 *
 * A trigger is defined after all data + constraints are committed-worthy, so a
 * CREATE TRIGGER failure must NOT roll the import back: on a statement failure
 * the phase records it (summary.failed), emits a diagnostic naming the file +
 * line, and CONTINUES to the next statement (a CUBRID statement failure does not
 * abort the transaction). When any statement failed, define_triggers () returns
 * PARTIAL and the run exits non-zero while the imported data + constraints + the
 * successfully-defined triggers still commit. A dump with no trigger file is
 * nothing to do (OK, 0 defined). The caller owns the transaction
 * (import_session.hpp): define_triggers () neither commits nor aborts.
 */

#ifndef _IMPORT_TRIGGERS_HPP_
#define _IMPORT_TRIGGERS_HPP_

#include "import_discovery.hpp"

#include <string>

namespace cubimport
{

  /*
   * The trigger-define outcome - the manifest [triggers] section source.
   * trigger_file is the <prefix>_trigger basename (empty when the dump carries
   * none); defined counts the trigger statements executed cleanly; failed counts
   * the statements that failed (recorded, the run exits non-zero).
   */
  struct trigger_summary
  {
    std::string trigger_file;
    int defined = 0;
    int failed = 0;
  };

  /*
   * Trigger-define outcome. OK means every trigger statement was defined, or the
   * dump carries no trigger file (nothing to do). PARTIAL means at least one
   * statement failed: the failures are recorded, the caller commits the imported
   * result + the successfully-defined triggers, and the run exits non-zero.
   * There is no hard-error/abort status - triggers are defined only after the
   * imported data + constraints are complete, so a failure never rolls them back.
   */
  enum class trigger_status
  {
    OK = 0,
    PARTIAL			/* one or more trigger statements failed; recorded, run exits non-zero */
  };

  /*
   * Define the deferred triggers from iset.trigger_file on the already-open
   * session by executing the file through the public one-statement-at-a-time
   * primitive, authorization disabled, skipping embedded transaction-boundary
   * statements. A statement that fails is recorded in summary.failed and the
   * phase continues. Records the outcome in summary (trigger_file / defined /
   * failed). Returns OK when every statement was defined or the dump has no
   * trigger file, or PARTIAL when at least one failed (the caller commits and
   * exits non-zero). The caller owns the transaction boundary.
   *
   * WU-50 resume: when resume is true (this is the phase a resumed run
   * re-enters, so an interrupted prior run may already have defined part of the
   * file) a statement that fails because the trigger already exists is counted
   * as defined instead of failed. The guard is the server's own answer rather
   * than a catalog pre-read, because this phase executes the file through the
   * parse-one-statement session and never holds a statement's text to take a
   * trigger name from.
   */
  trigger_status define_triggers (const import_set &iset, trigger_summary &summary, bool resume = false);

} // namespace cubimport

#endif /* _IMPORT_TRIGGERS_HPP_ */
