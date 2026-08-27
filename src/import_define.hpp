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
 * import_define.hpp - Stage 2 (Definition phase) of the importdb pipeline
 *
 * After Discovery rosters the dump (WU-11) and the manifest is written
 * (WU-12), the Definition phase executes the dump's definition DDL - classes,
 * columns, ADD SUPERCLASS, serials, partitions, and PK/UK/FK constraints - to
 * produce a fully-defined but empty target DB (10-design.md §2). Triggers
 * (WU-35) and secondary indexes (WU-32) are DEFERRED; no data is loaded
 * (WU-31). It reuses the same public DDL-execution primitive loaddb is built on
 * - db_make_session_for_one_statement_execution () plus the
 * db_parse_one_statement / db_compile_statement / db_execute_statement loop -
 * rather than loaddb's static wrappers, so importdb stays decoupled from loaddb
 * internals.
 *
 * define () assumes the target is already connected: the one CS-mode session is
 * opened once by import_session.hpp (session_open) and shared with the Graph
 * builder's catalog snapshot (WU-21) and the later strip/load/rebuild stages,
 * so the catalog stays consistent across the §4 lifecycle. define () commits
 * its DDL on success (leaving a durable defined-and-empty target) or aborts on
 * a statement error; the caller closes the session.
 */

#ifndef _IMPORT_DEFINE_HPP_
#define _IMPORT_DEFINE_HPP_

#include "import_discovery.hpp"

namespace cubimport
{

  /*
   * Definition-phase outcome. OK means the target DB is fully defined and
   * empty; ERR_DDL corresponds to a named diagnostic already emitted by
   * define ().
   */
  enum class define_status
  {
    OK = 0,
    ERR_DDL			/* a definition DDL statement failed */
  };

  /*
   * Execute the dump's definition DDL (schema + PK/UK/FK; triggers and
   * secondary indexes deferred) on the already-open session (session_open),
   * leaving a fully-defined empty target DB. Runs with authorization disabled
   * (DBA-group members) and triggers off. On success, when commit is true the
   * DDL is committed (a durable defined-and-empty target); when commit is false
   * (a --dry-run) the DDL is left uncommitted in the open transaction so the
   * caller's session_close(false) aborts it and the target is left completely
   * unchanged (CUBRID DDL is transactional). Either way the same catalog is
   * visible to the Graph builder's snapshot on this session. On a statement
   * error emits the matching named diagnostic, aborts the transaction, and
   * returns ERR_DDL. The caller owns connect/close.
   */
  define_status define (const import_set &iset, bool commit);

} // namespace cubimport

#endif /* _IMPORT_DEFINE_HPP_ */
