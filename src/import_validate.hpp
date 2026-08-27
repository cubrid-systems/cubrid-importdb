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
   * Validate every FK edge of the graph by anti-join against the loaded target on
   * the already-open session, honoring the fail-fast / --continue policy
   * (continue_on_error) and skipping edges whose parent PK the Rebuild phase
   * withheld (rebuild.withheld). Records the per-edge results + enumerated orphan
   * rows in summary. On a violation writes the offending rows to the exceptions
   * artifact in iset.dump_dir and returns VIOLATED; when every validatable edge
   * is clean returns OK. On a hard error (anti-join failed, artifact write
   * failed, or a malformed column mapping) emits the matching named diagnostic
   * and returns ERR_VALIDATE. The caller owns the transaction boundary.
   */
  validate_status validate_fks (const import_set &iset, const dependency_graph &graph, const rebuild_summary &rebuild,
				bool continue_on_error, validate_summary &summary);

  /*
   * WU-41 parallel variant. The per-edge anti-joins are independent READS on the
   * loaded tables (concurrent reads don't block), and on lineorder-dominated
   * schemas re-validation is the largest terminal cost (WU-42 probe), so this is
   * the terminal phase worth parallelizing. The CS client is one-connection-per-
   * process, so it runs each non-withheld edge's count-only anti-join as an
   * independent `csql -C` child, up to `degree` at a time. This is an OPTIMISTIC
   * fast path for the clean case only: if every edge counts 0 it builds the same
   * clean summary the serial path would; on ANY violation (count>0), csql/parse
   * error, malformed mapping, or empty worklist it delegates to serial
   * validate_fks () for the authoritative enumeration + exceptions artifact +
   * fail-fast/--continue reporting. The caller MUST have committed the rebuild
   * first (session_commit) so the independent csql connections see the rebuilt
   * parent PK indexes (index-backed anti-joins); user/password are forwarded to
   * each child's -u/-p. Result is identical to the serial path in every case.
   */
  validate_status validate_fks_parallel (const import_set &iset, const dependency_graph &graph,
					 const rebuild_summary &rebuild, bool continue_on_error, int degree,
					 const char *user, const char *password, validate_summary &summary);

} // namespace cubimport

#endif /* _IMPORT_VALIDATE_HPP_ */
