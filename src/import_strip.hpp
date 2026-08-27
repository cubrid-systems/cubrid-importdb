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
 * import_strip.hpp - Stage 4 (Strip phase, step 1 of the constraint lifecycle)
 *
 * After the Definition phase (WU-20) leaves a fully-defined empty target and the
 * Graph builder snapshots its per-node constraint inventory (WU-21), the Strip
 * phase DROPs those PK/UK/FK constraints on the still-empty tables so the data
 * phase (WU-31) loads into bare heaps (10-design.md §6 / 11-design-detail.md
 * §4.1 step 3, validated by the WU-04 prototype's strip_ddl()). It generates the
 * ALTER statements from the in-memory DependencyGraph - NOT from a file - and
 * executes them on the already-open session (import_session.hpp) with the same
 * public single-statement client primitive the Graph builder reads the catalog
 * with (db_execute), authorization disabled for the DBA-group path. loaddb is
 * untouched. No data is loaded (WU-31) and nothing is rebuilt (WU-32).
 *
 * The DROP order is the §4.1 global order (all of one kind before the next): all
 * FKs first (they depend on the PKs/uniques they reference), then all standalone
 * uniques, then all PKs. strip () records the dropped set in drop order - the
 * Gap-R resume basis and what the terminal rebuild (WU-32) re-adds - and leaves
 * the transaction open: the caller commits (session_close (true)) on success or
 * aborts (session_close (false)) on failure.
 */

#ifndef _IMPORT_STRIP_HPP_
#define _IMPORT_STRIP_HPP_

#include "import_graph.hpp"

#include <string>
#include <vector>

namespace cubimport
{

  /* import_resume.hpp - the WU-50 resume guard. Only ever taken by pointer
   * here, and declaring it keeps the include graph acyclic (import_resume.hpp
   * needs this header for stripped_constraint). */
  struct catalog_state;

  /* The kind of a dropped constraint (the rebuild is the reverse: PK/UK then
   * FK). PK is dropped by class (DROP PRIMARY KEY), so its name is recorded for
   * the rebuild but is not part of the DROP statement. */
  enum class stripped_kind
  {
    FK,
    UNIQUE,
    PK
  };

  /*
   * One stripped constraint: the class it was on, its kind, and the
   * index/constraint name (from the graph's per-node inventory). The recorded
   * list is the WU-32 rebuild / resume basis.
   */
  struct stripped_constraint
  {
    stripped_kind kind;
    std::string cls;
    std::string name;
  };

  /*
   * Strip-phase outcome. OK means every PK/UK/FK in the graph's inventory was
   * dropped and the target now holds bare heaps; ERR_STRIP corresponds to a
   * named diagnostic already emitted by strip ().
   */
  enum class strip_status
  {
    OK = 0,
    ERR_STRIP			/* a strip DDL statement failed */
  };

  /*
   * Drop every PK/UK/FK named in the DependencyGraph's per-node constraint
   * inventory from the (empty) defined target, in the §4.1 global order (all
   * FKs, then all standalone uniques, then all PKs), so the data phase loads
   * into bare heaps. A constraint is dropped only on the class that owns it: an
   * inherited copy on a subclass is skipped (the owner's drop clears the whole
   * inheritance chain). Runs on the already-open session with authorization
   * disabled (DBA-group path). On success appends every dropped constraint to
   * stripped in drop order (the rebuild / resume basis) and returns OK; the
   * caller owns the transaction boundary (commits on success). On a statement
   * failure emits the matching named diagnostic, clears stripped, and returns
   * ERR_STRIP; the caller aborts and closes the session.
   *
   * WU-50 resume: when present is non-null (this is the phase a resumed run
   * re-enters, so an interrupted prior strip may already have dropped part of
   * the inventory) a constraint the catalog no longer holds is NOT dropped
   * again - it is only recorded in stripped, because the rebuild input must
   * name every constraint that is gone, whichever run dropped it.
   */
  strip_status strip (const dependency_graph &graph, std::vector<stripped_constraint> &stripped,
		      const catalog_state *present = nullptr);

} // namespace cubimport

#endif /* _IMPORT_STRIP_HPP_ */
