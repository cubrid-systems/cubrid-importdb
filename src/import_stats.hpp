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
 * import_stats.hpp - Stage 4 (Statistics update, first terminal task of WU-35)
 *
 * After the FK define phase (WU-34) closes the §6 constraint lifecycle
 * (catalog == snapshot on a clean dump), the tables hold their data and every
 * PK/UK/FK/index is restored - but their class statistics are still whatever the
 * empty defined target had, because the Load phase built the load_args with
 * disable_statistics set (import_load.cpp). This phase refreshes the per-class
 * statistics so the query optimizer sees the real cardinality/histograms of the
 * imported data, reusing loaddb's OWN per-class stats path: for each user class
 * an sm_update_statistics (class, STATS_WITH_SAMPLING) - the exact call loaddb's
 * loader makes after a load (load_sa_loader.cpp ldr_update_statistics), sampling
 * rather than full-scan to match loaddb. It runs over graph.nodes, so classes the
 * Q11 gate skipped (--skip-object-classes) are already excluded.
 *
 * Statistics are NOT part of the data's correctness (the round-trip catalog is
 * already restored), so a per-class update failure is never fatal: it is
 * recorded in summary.failed, a diagnostic naming the class is emitted, and the
 * phase CONTINUES to the next class. When any class failed, update_stats ()
 * returns PARTIAL and the run exits non-zero (mirroring loaddb, which sets a
 * non-zero exit on a stats-update failure) while the imported data + constraints
 * still commit. The caller owns the transaction (import_session.hpp):
 * update_stats () neither commits nor aborts.
 */

#ifndef _IMPORT_STATS_HPP_
#define _IMPORT_STATS_HPP_

#include "import_discovery.hpp"
#include "import_graph.hpp"

#include <string>
#include <vector>

namespace cubimport
{

  /* One class whose statistics update failed: the class name and the surfaced
   * error text. Recorded in the manifest [stats] section (the run exits
   * non-zero) so an operator can re-run UPDATE STATISTICS on it. */
  struct stats_failure
  {
    std::string cls;
    std::string reason;
  };

  /*
   * The statistics-update outcome - the manifest [stats] section source.
   * updated counts the classes whose statistics refreshed cleanly; failed lists
   * the classes whose update failed with the surfaced reason.
   */
  struct stats_summary
  {
    int updated = 0;
    std::vector<stats_failure> failed;
  };

  /*
   * Statistics-update outcome. OK means every user class's statistics refreshed.
   * PARTIAL means at least one class failed: the failures are recorded, the
   * caller commits the imported result, and the run exits non-zero. There is no
   * hard-error/abort status - statistics are not part of the data's correctness,
   * so a failure never rolls back the imported data.
   */
  enum class stats_status
  {
    OK = 0,
    PARTIAL			/* one or more classes failed; recorded, run exits non-zero */
  };

  /*
   * Refresh the per-class statistics of every graph node on the already-open
   * session, reusing loaddb's per-class path (sm_update_statistics with
   * sampling), authorization disabled for the DBA-group path. A class whose
   * update fails is recorded in summary.failed and the phase continues. Records
   * the outcome in summary (updated / failed). Returns OK when every class
   * refreshed, or PARTIAL when at least one failed (the caller commits and exits
   * non-zero). The caller owns the transaction boundary.
   */
  stats_status update_stats (const import_set &iset, const dependency_graph &graph, stats_summary &summary);

} // namespace cubimport

#endif /* _IMPORT_STATS_HPP_ */
