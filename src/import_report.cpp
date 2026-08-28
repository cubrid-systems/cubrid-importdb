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
 * import_report.cpp - Stage 5 (Reporter v1) of the importdb pipeline
 *
 * Prints the consolidated RunReport (10-design.md §7) at the end of a completed
 * run. A PURE pass over the in-memory artifacts the earlier stages produced -
 * ImportSet, DependencyGraph, Schedule, and the per-phase summaries (load /
 * rebuild / validate / fkdefine / stats / triggers) - so it makes no server call
 * and cannot fail. The report answers G5 from the output alone: which classes are
 * done / pending / skipped, what an operator must repair (pending rebuilds +
 * withheld FKs, with re-add DDL), and how many rows landed. The header + one-line
 * summary are message-catalog strings; the structured body is printed as plain
 * detail lines, mirroring print_schedule () / print_graph_summary ().
 */

#include "import_report.hpp"
#include "import_progress.hpp"

#include "import_discovery.hpp"
#include "import_graph.hpp"
#include "import_plan.hpp"
#include "import_load.hpp"
#include "import_rebuild.hpp"
#include "import_validate.hpp"
#include "import_fkdefine.hpp"
#include "import_stats.hpp"
#include "import_triggers.hpp"

#include "utility.h"
#include "message_catalog.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace
{
  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  /* "[a, b, c]" - one data-level's classes, order preserved. */
  std::string
  join_level (const std::vector<std::string> &items)
  {
    std::string out = "[";
    for (size_t i = 0; i < items.size (); i++)
      {
	if (i != 0)
	  {
	    out += ", ";
	  }
	out += items[i];
      }
    out += "]";
    return out;
  }

  /* The kind keyword for a pending PK/UNIQUE rebuild record. */
  const char *
  stripped_kind_name (cubimport::stripped_kind kind)
  {
    switch (kind)
      {
      case cubimport::stripped_kind::FK:
	return "fk";
      case cubimport::stripped_kind::UNIQUE:
	return "unique";
      case cubimport::stripped_kind::PK:
	return "pk";
      }
    return "";
  }
} // namespace

namespace cubimport
{

  void
  print_report (const import_set &iset, const dependency_graph &graph, const schedule &sched,
		const load_summary &load, const rebuild_summary &rebuild, const validate_summary &validate,
		const fkdefine_summary &fkdefine, const stats_summary &stats, const trigger_summary &triggers)
  {
    /* A class is "pending" (an operator must finish it) when its own PK/UNIQUE
     * failed to rebuild, a validation orphan left its FK undefined, or a cascade
     * withheld its FK. A statistics failure is a done-with-a-note, not pending. */
    std::set<std::string> pending;
    for (const pending_rebuild &p : rebuild.pending)
      {
	pending.insert (p.cls);
      }
    for (const withheld_define &w : fkdefine.withheld)
      {
	pending.insert (w.child);
      }
    for (const fk_edge_result &e : validate.edges)
      {
	if (!e.skipped && e.orphans > 0)
	  {
	    pending.insert (e.child);
	  }
      }
    std::set<std::string> stats_stale;
    for (const stats_failure &f : stats.failed)
      {
	stats_stale.insert (f.cls);
      }

    const bool partial = !rebuild.pending.empty () || validate.violated_edges > 0 || !fkdefine.withheld.empty ()
			 || !stats.failed.empty () || triggers.failed > 0;
    const char *verdict = partial ? "PARTIAL" : "COMPLETE";

    IMPORT_PRINT (msg (IMPORTDB_MSG_REPORT_HEADER), iset.database_name.c_str (), verdict);

    /* planned data order - the Schedule's level sets. */
    IMPORT_PRINT ("  planned data order (%d level(s)):\n", (int) sched.data_levels.size ());
    for (size_t i = 0; i < sched.data_levels.size (); i++)
      {
	IMPORT_PRINT ("      L%d: %s\n", (int) i, join_level (sched.data_levels[i]).c_str ());
      }

    /* per-class state (graph.nodes is sorted by class name). */
    IMPORT_PRINT ("  classes (%d imported, %d skipped):\n", (int) graph.nodes.size (),
	     (int) graph.skipped_classes.size ());
    for (const graph_node &n : graph.nodes)
      {
	const char *state = pending.count (n.name) ? "pending" : "done";
	const char *note = stats_stale.count (n.name) ? "  [statistics stale - update failed]" : "";
	IMPORT_PRINT ("      %-8s %s%s\n", state, n.name.c_str (), note);
      }
    for (const std::string &c : graph.skipped_classes)
      {
	IMPORT_PRINT ("      %-8s %s  [object-valued; excluded by --skip-object-classes]\n", "skipped", c.c_str ());
      }

    /* loaded rows - total then per object file. */
    IMPORT_PRINT ("  loaded: %ld row(s) from %d object file(s)\n", (long) load.total_rows, load.loaded_files);
    for (const load_file_result &f : load.files)
      {
	if (f.failed > 0)
	  {
	    IMPORT_PRINT ("      %s: %ld row(s), %ld failed\n", f.object_file.c_str (), (long) f.rows,
		     (long) f.failed);
	  }
	else
	  {
	    IMPORT_PRINT ("      %s: %ld row(s)\n", f.object_file.c_str (), (long) f.rows);
	  }
      }

    /* pending-rebuild record (§6 rebuild-failure contract): the PK/UNIQUE whose
     * terminal rebuild failed. re-add is from the dump's own PK/UK statement. */
    if (!rebuild.pending.empty ())
      {
	IMPORT_PRINT ("  pending rebuilds (%d) -- re-add from the dump's schema, then re-run:\n",
		 (int) rebuild.pending.size ());
	for (const pending_rebuild &p : rebuild.pending)
	  {
	    IMPORT_PRINT ("      %s %s [%s] :: %s\n", stripped_kind_name (p.kind), p.cls.c_str (), p.name.c_str (),
		     p.reason.c_str ());
	    IMPORT_PRINT ("        re-add: %s\n", p.readd_ddl.c_str ());
	  }
      }

    /* failed deferred indexes: non-unique, so never the duplicate-key path - a
     * disk-full or a btree_load_index error. They make the run PARTIAL, so the
     * operator has to be told which index is missing (and a resumed run has to
     * say so too, which is why the manifest carries them). */
    if (!rebuild.failed_indexes.empty ())
      {
	IMPORT_PRINT ("  indexes not built (%d) -- re-create from the dump's index file:\n",
		 (int) rebuild.failed_indexes.size ());
	for (const failed_index &f : rebuild.failed_indexes)
	  {
	    IMPORT_PRINT ("      %s :: %s\n", f.name.c_str (), f.reason.c_str ());
	  }
      }

    /* withheld-FK record: the FK left undefined (validation orphan or parent key
     * un-rebuilt), each with the exact re-add DDL an operator runs after repair. */
    if (!fkdefine.withheld.empty ())
      {
	IMPORT_PRINT ("  withheld FKs (%d) -- defined after the data is repaired:\n",
		 (int) fkdefine.withheld.size ());
	for (const withheld_define &w : fkdefine.withheld)
	  {
	    IMPORT_PRINT ("      %s -> %s (%s) :: %s\n", w.child.c_str (), w.parent.c_str (), w.name.c_str (),
		     w.reason.c_str ());
	    IMPORT_PRINT ("        re-add: %s\n", w.readd_ddl.c_str ());
	  }
      }

    /* Where the machine-readable records live (offending rows + full re-add DDL). */
    if (!validate.exceptions_file.empty ())
      {
	IMPORT_PRINT ("  offending rows enumerated in: %s\n", validate.exceptions_file.c_str ());
      }

    const int skipped = (int) graph.skipped_classes.size ();
    const int pending_cnt = (int) pending.size ();
    const int done = (int) graph.nodes.size () - pending_cnt;
    IMPORT_PRINT (msg (IMPORTDB_MSG_REPORT_SUMMARY), iset.database_name.c_str (), verdict, done, skipped,
	     pending_cnt, (int) fkdefine.withheld.size (), (long) load.total_rows, iset.dump_dir.c_str ());
  }

} // namespace cubimport
