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
 * import_validate.hpp - Stage 4 (FK re-validation, step 4 of the constraint lifecycle)
 *
 * After the Rebuild phase (WU-32) restores the PK/UNIQUE constraints and plain
 * indexes on the loaded target, the FK re-validation phase checks that every FK
 * edge's child rows actually reference an existing parent (10-design.md §6 /
 * §4.5). CUBRID's ADD FOREIGN KEY does NOT validate the existing rows, so this
 * phase does the validation itself, BEFORE the FK is defined (FK define is
 * WU-34): for each FK edge child -> parent it runs a set-based anti-join -
 *   SELECT <child pk/fk cols> FROM [child] AS c
 *   WHERE <every fk col> IS NOT NULL
 *     AND NOT EXISTS (SELECT 1 FROM [parent] AS p WHERE p.<pk col> = c.<fk col> [AND ...])
 * - which returns exactly the orphan child rows (an FK value with no matching
 * parent PK). The child FK columns are paired with the parent PK columns
 * POSITIONALLY (FK key order <-> PK key order), from the mapping the Graph
 * builder captured at snapshot (fk_edge.child_columns / parent_pk_columns);
 * composite FKs pair several columns. loaddb is not touched.
 *
 * Policy (WU-10 D4): the default is fail-fast - validate the edges in a
 * deterministic order and stop at the FIRST edge with an orphan, recording only
 * that edge's offenders. --continue validates every (non-withheld) edge and
 * enumerates every offending row across all violated edges. Either way, any
 * orphan makes the run exit non-zero. An FK edge whose parent PK failed to
 * rebuild (WU-32's withheld set) cannot be validated (no parent PK index) and is
 * skipped - not counted as a new failure. On any violation the offending rows
 * are written to the exceptions artifact in the importdb directory (§4.4) for an
 * operator to repair from; WU-34 later withholds these edges' FK. The caller
 * owns the transaction (import_session.hpp): validate_fks () neither commits nor
 * aborts.
 */

#ifndef _IMPORT_VALIDATE_HPP_
#define _IMPORT_VALIDATE_HPP_

#include "import_discovery.hpp"
#include "import_graph.hpp"
#include "import_rebuild.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cubimport
{

  /*
   * One orphan child row found by an FK edge's anti-join: the edge it violated
   * (child -> parent, fk name), the child's identifying key (its PK columns as
   * "col=value", comma-joined; "(no pk)" when the child has no PK), and the
   * offending FK column value(s) ("col=value", comma-joined). This is one line of
   * the exceptions artifact.
   */
  struct fk_orphan
  {
    std::string child;
    std::string parent;
    std::string fk_name;
    std::string child_key;
    std::string fk_values;
  };

  /*
   * The per-edge validation outcome for the manifest [validate] section. skipped
   * is true when the edge's parent PK was withheld by the Rebuild phase (it could
   * not be validated); otherwise orphans is the offending-row count (0 = clean).
   * When fail-fast stops early, the edges after the first violated one are left
   * unvalidated and are not recorded.
   */
  struct fk_edge_result
  {
    std::string child;
    std::string parent;
    std::string name;
    bool skipped = false;
    int64_t orphans = 0;
  };

  /*
   * The FK re-validation outcome record - the manifest [validate] section source.
   * edges lists each edge's result in validation (deterministic) order; orphans
   * is the enumerated offending rows written to the exceptions artifact (the
   * first violated edge's rows under fail-fast, every violated edge's rows under
   * --continue); violated_edges / total_orphans are the tallies; exceptions_file
   * is the artifact basename (empty when no violation was found).
   */
  struct validate_summary
  {
    std::vector<fk_edge_result> edges;
    std::vector<fk_orphan> orphans;
    int validated_edges = 0;
    int skipped_edges = 0;
    int violated_edges = 0;
    int64_t total_orphans = 0;
    std::string exceptions_file;
  };

  /*
   * FK re-validation outcome. OK means every validatable edge is clean (no FK is
   * defined yet - that is WU-34). VIOLATED means at least one edge had orphan
   * rows: the offenders are recorded in summary and written to the exceptions
   * artifact, and the caller commits the loaded data + rebuilt constraints but
   * exits non-zero (FK simply never defined on the violated data). ERR_VALIDATE
   * is a hard error already reported by validate_fks () (an anti-join query
   * failed, the exceptions artifact could not be written, or an FK edge's column
   * mapping is missing) - the caller aborts.
   */
  enum class validate_status
  {
    OK = 0,
    VIOLATED,			/* orphan rows found; recorded + artifact written; run exits non-zero */
    ERR_VALIDATE		/* anti-join query / artifact write / mapping error */
  };

  /*
   * Enumerate ONE FK edge's orphan child rows by anti-join against the loaded
   * target on the already-open session, appending them to `out`.
   *
   * This used to be a phase of its own, run over every edge before any FK was
   * defined. It is not, any more, and the reason is worth stating: the engine
   * already validates. `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` builds the
   * FK's b-tree over the existing rows (sm_add_constraint -> btree_load_index) and
   * `btree_load_check_fk` probes every key against the parent's PK index while it
   * does, so an orphan makes the statement fail with ER_FK_INVALID and the FK is
   * not created. Measured on 11.5.0.2494 and on develop d0b290459; a separate
   * anti-join pass over clean data was pure cost -- ~0.56 s per edge against
   * ~0.2 s for the ADD itself once the child's pages are warm, which they are
   * right after the load.
   *
   * What the engine does NOT do is enumerate: it reports the FIRST offending value
   * and stops. So the anti-join survives as the FAILURE path -- run for the one
   * edge the engine just rejected, to list every offending row for the exceptions
   * artifact. Clean dumps never pay for it.
   *
   * Returns NO_ERROR, or a negative error code after emitting the named
   * diagnostic (a malformed column mapping, or the anti-join query failing).
   * Manages its own authorization window; nesting inside the caller's is safe.
   */
  int enumerate_fk_orphans (const fk_edge &edge, std::vector<fk_orphan> &out);

  /*
   * Write the exceptions artifact for the violations recorded in summary. Called
   * by the FK define phase once it knows which edges the engine rejected. The
   * file is truncated, so exactly one current copy of each record exists; the FK
   * define phase then appends its own withheld-FK section after this.
   *
   * Returns false on a write failure (the caller reports and aborts).
   */
  bool write_fk_exceptions (const std::string &path, const import_set &iset, const dependency_graph &graph,
			    const validate_summary &summary, bool continue_on_error);

} // namespace cubimport

#endif /* _IMPORT_VALIDATE_HPP_ */
