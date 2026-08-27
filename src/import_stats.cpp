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
 * import_stats.cpp - Stage 4 (Statistics update, first terminal task of WU-35)
 *
 * Refreshes the per-class statistics of the imported target after the FK define
 * phase (WU-34) restored the constraints, so the query optimizer sees the real
 * cardinality/histograms of the loaded data (the Load phase built its load_args
 * with disable_statistics set - import_load.cpp - deferring statistics to here).
 * It reuses loaddb's OWN per-class stats path: for each user class an
 * sm_update_statistics (class, STATS_WITH_SAMPLING) - the exact call loaddb's
 * loader makes after a load (load_sa_loader.cpp ldr_update_statistics uses
 * STATS_WITH_SAMPLING, not full-scan). The iteration is over graph.nodes, so a
 * class the Q11 gate skipped (--skip-object-classes) is already excluded.
 *
 * Statistics are not part of the data's correctness (the round-trip catalog is
 * already restored by WU-34), so a per-class failure is never fatal: it is
 * recorded in summary.failed, a diagnostic names the class, and the phase
 * continues. When any class failed update_stats () returns PARTIAL and the run
 * exits non-zero (as loaddb sets a non-zero exit on a stats-update failure) while
 * the imported data + constraints still commit. Authorization is disabled for
 * the DBA-group path, as the Definition/Rebuild phases do. The caller owns the
 * transaction (import_session.hpp): update_stats () neither commits nor aborts.
 */

#include "import_stats.hpp"

#include "db.h"
#include "authenticate.h"
#include "error_manager.h"
#include "schema_manager.h"
#include "statistics.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <string>

namespace
{
  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }
} // namespace

namespace cubimport
{

  stats_status
  update_stats (const import_set &iset, const dependency_graph &graph, stats_summary &summary)
  {
    summary = stats_summary ();

    /* As the Definition/Rebuild phases do, DBA-group members refresh statistics
     * with authorization disabled (the dump's classes may span several owners). */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    for (const graph_node &n : graph.nodes)
      {
	DB_OBJECT *classop = db_find_class (n.name.c_str ());
	int error = (classop != NULL) ? sm_update_statistics (classop, STATS_WITH_SAMPLING) : ER_FAILED;
	if (error != NO_ERROR)
	  {
	    /* Non-fatal: record the class + reason and continue. db_find_class /
	     * sm_update_statistics has set the error; capture its text once. */
	    const std::string reason = db_error_string (3);
	    summary.failed.push_back ({ n.name, reason });
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_STATS_CLASS_FAILED), n.name.c_str (), reason.c_str ());
	    continue;
	  }
	summary.updated++;
      }

    AU_RESTORE (au_save);

    if (!summary.failed.empty ())
      {
	fprintf (stdout, msg (IMPORTDB_MSG_STATS_PARTIAL), summary.updated, (int) summary.failed.size (),
		 iset.database_name.c_str ());
	return stats_status::PARTIAL;
      }

    fprintf (stdout, msg (IMPORTDB_MSG_STATS_COMPLETE), summary.updated, iset.database_name.c_str ());
    return stats_status::OK;
  }

} // namespace cubimport
