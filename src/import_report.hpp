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
 * import_report.hpp - Stage 5 (Reporter v1) of the importdb pipeline
 *
 * After the Executor's terminal tasks complete (WU-30..35) the Reporter prints a
 * single consolidated RunReport (10-design.md §7) so an operator can answer, from
 * the output alone (G5 acceptance), which classes are done / pending / skipped,
 * what was left for them to repair, and how many rows landed. It is a PURE pass
 * over the in-memory artifacts the earlier stages already produced (ImportSet,
 * DependencyGraph, Schedule, and the per-phase summaries) - no server call, no
 * new state - so it runs after session_close () and cannot fail.
 *
 * The v1 (serial) report carries the §7 RunReport contract minus resume hints
 * (Gap-R is WU-50):
 *   - planned data order        (the Schedule's level sets)
 *   - per-class state           (done | pending | skipped; a stats-stale note)
 *   - loaded rows               (total + per object file)
 *   - pending-rebuild record    (PK/UNIQUE whose terminal rebuild failed + reason + exact re-add DDL)
 *   - withheld-FK record        (FK left undefined + reason + exact re-add DDL)
 *   - skipped-class record      (the Q11 object-valued classes excluded)
 *   - a one-line G5 summary      (done / skipped / pending / withheld + row total)
 *
 * A class is "pending" (an operator must finish it) when its PK/UNIQUE is in the
 * Rebuild phase's pending set, a validation orphan left its FK undefined, or a
 * cascade withheld its FK; every other imported class is "done" (a failed
 * statistics update is a done-with-a-note, not pending - the data + constraints
 * are complete). "in-flight" and "failed" from the §7 state set do not occur in a
 * serial run observed at its close (in-flight is the M4 parallel case; a hard
 * failure aborts before the report). The verdict is COMPLETE when nothing is
 * pending/withheld/violated/failed, else PARTIAL - matching the process exit code.
 */

#ifndef _IMPORT_REPORT_HPP_
#define _IMPORT_REPORT_HPP_

namespace cubimport
{

  struct import_set;		/* import_discovery.hpp */
  struct dependency_graph;	/* import_graph.hpp */
  struct schedule;		/* import_plan.hpp */
  struct load_summary;		/* import_load.hpp */
  struct rebuild_summary;	/* import_rebuild.hpp */
  struct validate_summary;	/* import_validate.hpp */
  struct fkdefine_summary;	/* import_fkdefine.hpp */
  struct stats_summary;		/* import_stats.hpp */
  struct trigger_summary;	/* import_triggers.hpp */

  /*
   * Print the consolidated RunReport for a completed (fully or partially) run to
   * stdout, aggregating the pipeline artifacts + per-phase summaries. Pure output:
   * makes no server call and returns nothing. Called on the normal path after the
   * one session is committed and closed; the dry-run and hard-error paths do not
   * reach it (they print their own terminal message).
   *
   * catalog_verified is the caller's closing check that the target really holds
   * what the phase records claim. False makes the verdict PARTIAL: a record that
   * disagrees with the catalog is not a complete import.
   */
  void print_report (const import_set &iset, const dependency_graph &graph, const schedule &sched,
		     const load_summary &load, const rebuild_summary &rebuild, const validate_summary &validate,
		     const fkdefine_summary &fkdefine, const stats_summary &stats, const trigger_summary &triggers,
		     bool catalog_verified);

} // namespace cubimport

#endif /* _IMPORT_REPORT_HPP_ */
