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
 * import_plan.hpp - Stage 3 (Planner) of the importdb pipeline
 *
 * The Planner turns the in-memory DependencyGraph (WU-21) into a Schedule
 * (10-design.md §5): the data-phase level sets (the parallel-eligible batches,
 * consumed straight from the graph) plus a terminal task list - the post-data
 * tasks (rebuild PK/unique, build deferred indexes, FK re-validate, FK define,
 * stats, define triggers) with explicit intra-terminal ordering edges. It is
 * PURE COMPUTATION on the graph: it issues no server calls and executes
 * nothing; the Executor consumes the Schedule in M3.
 *
 * The ordering constraints (10-design.md §5/§6 phase order) are represented as
 * explicit prerequisite edges between terminal tasks (task -> prerequisite), so
 * they can be machine-checked: validate_schedule () confirms the task graph is
 * acyclic, that a topological order exists, and that the emitted (printed) task
 * order respects every prerequisite. A violated constraint is a logic bug.
 */

#ifndef _IMPORT_PLAN_HPP_
#define _IMPORT_PLAN_HPP_

#include "import_graph.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cubimport
{

  /*
   * The kinds of terminal (post-data) task, in phase order (10-design.md §6):
   * rebuild PK/unique + plain indexes -> FK re-validate -> FK define -> stats
   * -> define triggers (last). The task list is the SAME for the DEFAULT and
   * SPLIT dump layouts (both converge on this order).
   */
  enum class terminal_task_kind
  {
    REBUILD_PK,			/* re-add the stripped/deferred primary key (per class w/ a PK) */
    REBUILD_UNIQUE,		/* re-add a standalone-unique constraint */
    BUILD_INDEX,		/* build the deferred plain CREATE INDEXes (<prefix>_indexes) */
    FK_VALIDATE,		/* the per-edge anti-join */
    FK_DEFINE,			/* define the FK after its validate (clean only, gated in M3) */
    STATS,			/* update statistics per class */
    DEFINE_TRIGGERS		/* define the deferred triggers (strictly last) */
  };

  /*
   * One terminal task. cls is the class it acts on (the child, for FK tasks;
   * empty for BUILD_INDEX which spans the whole indexes file); parent is the FK
   * parent (FK tasks only); name is the constraint / index / edge name (or the
   * index file basename for BUILD_INDEX, or empty for STATS/DEFINE_TRIGGERS).
   * prereqs holds indices into schedule.terminal_tasks that MUST be emitted
   * before this task - the intra-terminal ordering edges (task -> prerequisite).
   */
  struct terminal_task
  {
    terminal_task_kind kind;
    std::string cls;
    std::string parent;
    std::string name;
    std::vector<std::size_t> prereqs;
  };

  /*
   * The Schedule (10-design.md §5). data_levels is the data-phase layering
   * (consumed from dependency_graph.level_sets - the parallel-eligible batches);
   * terminal_tasks is the post-data task list, already serialized in a valid
   * topological order (the vector index order is the emitted order), each task
   * carrying its prerequisite edges for the machine-check.
   */
  struct schedule
  {
    std::string database_name;
    std::vector<std::vector<std::string>> data_levels;
    std::vector<terminal_task> terminal_tasks;
  };

  /*
   * Build outcome. OK means the Schedule was built and passed its own
   * machine-check (validate_schedule); ERR_INVALID_SCHEDULE means the produced
   * Schedule violated its declared ordering - a logic bug already reported by
   * validate_schedule ().
   */
  enum class build_schedule_status
  {
    OK = 0,
    ERR_INVALID_SCHEDULE
  };

  /*
   * Build the Schedule from the DependencyGraph + the ImportSet (for the index/
   * trigger artifacts). Consumes graph.level_sets as the data phase, derives the
   * terminal task list + its ordering edges (10-design.md §5/§6), then
   * machine-checks it via validate_schedule (). Returns OK on success, or
   * ERR_INVALID_SCHEDULE (already reported) if the check fails. Issues no server
   * calls - pure computation on the in-memory graph.
   */
  build_schedule_status build_schedule (const dependency_graph &graph, const import_set &iset, schedule &sched);

  /*
   * Machine-check the Schedule against its own declared ordering: build the task
   * dependency graph from the prerequisite edges, confirm it is acyclic and a
   * topological order exists (Kahn), and confirm the emitted task order (the
   * vector order) respects every edge (no task before a prerequisite). Returns
   * true when the Schedule is sound; on a violation emits the named diagnostic
   * (IMPORTDB_MSG_SCHEDULE_INVALID) and returns false. This is the "ordering
   * constraints machine-checked" acceptance line.
   */
  bool validate_schedule (const schedule &sched);

  /*
   * Print the Schedule to stdout: the planned data-level order, then the
   * terminal tasks in their (valid) emitted order, labeled by type with each
   * task's prerequisites shown - the dry-run material WU-23 consumes later.
   */
  void print_schedule (const schedule &sched);

} // namespace cubimport

#endif /* _IMPORT_PLAN_HPP_ */
