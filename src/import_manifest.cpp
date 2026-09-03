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
 * import_manifest.cpp - The importdb manifest (plan/progress record), v0
 *
 * Renders the ImportSet into a line-based, human-readable text manifest and
 * writes it into the importdb directory (foundation §4.4). The format is a
 * regular INI-like shape - comment lines (#), a scalar preamble, and a few
 * [section] blocks whose body lines are all "key: value" - so a future resume
 * (WU-50) can re-read it, and later WUs can add keys/sections without breaking
 * older readers. Only the fields that exist at v0 are populated: the rostered
 * set, an explicit planned-order placeholder (topological order is WU-22), and
 * the phase markers with only "discovered" reached.
 *
 * The write is crash-safe: the content is rendered in full, written to a temp
 * file in the same directory, fsync'd, then rename()'d into place. rename() is
 * atomic on POSIX, so the manifest path only ever holds a complete file - a
 * kill mid-write leaves at most a stale temp file, never a half-written
 * manifest. Later per-phase updates reuse the same atomic-rename.
 */

#include "import_manifest.hpp"
#include "import_graph.hpp"
#include "import_plan.hpp"
#include "import_strip.hpp"
#include "import_load.hpp"
#include "import_rebuild.hpp"
#include "import_validate.hpp"
#include "import_fkdefine.hpp"
#include "import_stats.hpp"
#include "import_triggers.hpp"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
  /* Basename of the importdb-written manifest inside the importdb directory.
   * Chosen so it neither starts with a dump <prefix>_ nor ends with an
   * unloaddb artifact suffix - so a later Discovery re-scan (resume) tolerates
   * it - and so it will not collide with a future exceptions artifact (WU-33). */
  const char *const MANIFEST_BASENAME = "importdb.manifest";

  /* Bumped when the on-disk format changes incompatibly; readers key off it.
   * v2 (WU-50 increment B review): the [strip] and [rebuild] record lines moved
   * from space- to TAB-delimited fields (a CUBRID identifier may contain a
   * space, and the space form silently mis-split such a class into the wrong
   * class+constraint pair), and two records that existed only in memory are now
   * written - the pending rebuild's re-add DDL and the failed plain indexes. A
   * reader refuses a manifest whose version is higher than this. */
  /* v3 dropped the VALIDATED phase: FK re-validation merged into FK define, since
 * the engine validates while building the FK. A v2 manifest names a phase this
 * build no longer has, so a resume from one is refused rather than misread. */
  const int MANIFEST_FORMAT_VERSION = 3;

  const char *const NONE_MARKER = "(none)";
  const char *const PLANNED_ORDER_PLACEHOLDER = "(placeholder: topological order assigned in WU-22)";

  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  std::string
  path_join (const std::string &dir, const std::string &name)
  {
    if (dir.empty () || dir.back () == '/')
      {
	return dir + name;
      }
    return dir + "/" + name;
  }

  /* Comma-separated join, preserving order (used for the split apply order and
   * the object roster). */
  std::string
  join_list (const std::vector<std::string> &items)
  {
    std::string out;
    for (size_t i = 0; i < items.size (); i++)
      {
	if (i != 0)
	  {
	    out += ", ";
	  }
	out += items[i];
      }
    return out;
  }

  std::string
  utc_timestamp ()
  {
    time_t now = time (NULL);
    struct tm tm_buf;
    char buf[32] = "";
    if (gmtime_r (&now, &tm_buf) != NULL)
      {
	strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
      }
    return std::string (buf);
  }

  /* The phase-block rows, in pipeline order. Indexed by import_phase value. */
  const char *const PHASE_NAMES[] = { "discovered", "defined", "stripped", "loaded", "rebuilt",
				      "fk_defined", "stats_updated", "done"
				    };
  /* The table is indexed by the enum, so a phase added or removed without
   * touching this array would silently shift every name. */
  static_assert (sizeof (PHASE_NAMES) / sizeof (PHASE_NAMES[0])
		 == (size_t) cubimport::import_phase::DONE + 1, "PHASE_NAMES must match import_phase");

  /* Render one FK cycle as "a -> b -> a" (the node list joined by " -> "). */
  std::string
  join_cycle (const std::vector<std::string> &cycle)
  {
    std::string out;
    for (size_t i = 0; i < cycle.size (); i++)
      {
	if (i != 0)
	  {
	    out += " -> ";
	  }
	out += cycle[i];
      }
    return out;
  }

  /*
   * Append the [graph] section: the DependencyGraph snapshot counts (nodes /
   * FK / inheritance / serial edges), detected FK cycles, and any skipped
   * (object-valued) classes. Written at the DEFINED phase once the Graph
   * builder has run (WU-21).
   */
  void
  render_graph_section (std::ostringstream &os, const cubimport::dependency_graph &graph)
  {
    size_t serial_edges = 0;
    for (const cubimport::graph_node &n : graph.nodes)
      {
	serial_edges += n.serials.size ();
      }

    os << "[graph]\n";
    os << "nodes: " << graph.nodes.size () << "\n";
    os << "fk_edges: " << graph.fk_edges.size () << "\n";
    os << "inherit_edges: " << graph.inherit_edges.size () << "\n";
    os << "serial_edges: " << serial_edges << "\n";
    if (graph.cycles.empty ())
      {
	os << "cycles: " << NONE_MARKER << "\n";
      }
    else
      {
	for (const std::vector<std::string> &cy : graph.cycles)
	  {
	    os << "cycle: " << join_cycle (cy) << "\n";
	  }
      }
    os << "skipped_classes: " << (graph.skipped_classes.empty () ? NONE_MARKER : join_list (graph.skipped_classes))
       << "\n";

    /* WU-50 resume basis: the full graph, tab-delimited detail lines (the count
     * lines above are for humans). After STRIP the catalog is bare, so a resume
     * cannot rebuild the graph from the catalog - it reads it back from here.
     * Field order is fixed; lists are comma-joined; a tab never occurs in a
     * CUBRID identifier so it is an unambiguous field separator.
     *   gnode: <name> \t cs=<0|1> \t part=<0|1> \t pk=<name> \t uk=<a,b> \t fk=<a,b> \t partn=<p1,p2> \t ser=<s1,s2>
     *   gfk:   <child> \t <parent> \t <name> \t cc=<c1,c2> \t ppk=<p1,p2> \t cpk=<k1,k2>
     *   ginh:  <child> \t <super>
     * node.constraints is serialized here (self-contained) even though [strip]
     * also lists them; the two are written together and agree. */
    for (const cubimport::graph_node &n : graph.nodes)
      {
	os << "gnode: " << n.name << "\t" << "cs=" << (n.cs_loadable ? 1 : 0) << "\t" << "part="
	   << (n.partitioned ? 1 : 0) << "\t" << "pk=" << n.constraints.pk << "\t" << "uk="
	   << join_list (n.constraints.unique) << "\t" << "fk=" << join_list (n.constraints.fk) << "\t" << "partn="
	   << join_list (n.partitions) << "\t" << "ser=" << join_list (n.serials) << "\n";
      }
    for (const cubimport::fk_edge &e : graph.fk_edges)
      {
	os << "gfk: " << e.child << "\t" << e.parent << "\t" << e.name << "\t" << "cc="
	   << join_list (e.child_columns) << "\t" << "ppk=" << join_list (e.parent_pk_columns) << "\t" << "cpk="
	   << join_list (e.child_pk_columns) << "\n";
      }
    for (const cubimport::inherit_edge &e : graph.inherit_edges)
      {
	os << "ginh: " << e.child << "\t" << e.super << "\n";
      }
    os << "\n";
  }

  /* The " | "-separated level order (classes within a level comma-joined) - the
   * flattened parallel-eligible data-phase order the Planner produced. */
  std::string
  join_levels (const std::vector<std::vector<std::string>> &levels)
  {
    std::string out;
    for (size_t i = 0; i < levels.size (); i++)
      {
	if (i != 0)
	  {
	    out += " | ";
	  }
	out += join_list (levels[i]);
      }
    return out;
  }

  /*
   * Append the [plan] section from the Planner's Schedule (WU-22): the real
   * data-level order (replacing the WU-12 placeholder) plus the terminal task
   * counts by kind. Written once the Planner has run (DEFINED phase).
   */
  void
  render_plan_section (std::ostringstream &os, const cubimport::schedule &sched)
  {
    int rebuild_pk = 0, rebuild_unique = 0, build_index = 0, fk_define = 0, stats = 0,
	define_triggers = 0;
    for (const cubimport::terminal_task &t : sched.terminal_tasks)
      {
	switch (t.kind)
	  {
	  case cubimport::terminal_task_kind::REBUILD_PK:
	    rebuild_pk++;
	    break;
	  case cubimport::terminal_task_kind::REBUILD_UNIQUE:
	    rebuild_unique++;
	    break;
	  case cubimport::terminal_task_kind::BUILD_INDEX:
	    build_index++;
	    break;
	  case cubimport::terminal_task_kind::FK_DEFINE:
	    fk_define++;
	    break;
	  case cubimport::terminal_task_kind::STATS:
	    stats++;
	    break;
	  case cubimport::terminal_task_kind::DEFINE_TRIGGERS:
	    define_triggers++;
	    break;
	  }
      }

    os << "[plan]\n";
    os << "planned_order: " << (sched.data_levels.empty () ? std::string (NONE_MARKER) : join_levels (sched.data_levels))
       << "\n";
    os << "data_levels: " << sched.data_levels.size () << "\n";
    os << "terminal_tasks: " << sched.terminal_tasks.size () << "\n";
    os << "rebuild_pk: " << rebuild_pk << "\n";
    os << "rebuild_unique: " << rebuild_unique << "\n";
    os << "build_index: " << build_index << "\n";
    os << "fk_define: " << fk_define << "\n";
    os << "stats: " << stats << "\n";
    os << "define_triggers: " << define_triggers << "\n";
    os << "\n";
  }

  /* The kind label of a stripped constraint, for the [strip] section lines. */
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

  /*
   * Append the [strip] section from the Strip phase (WU-30): the dropped
   * constraint counts by kind, then one TAB-delimited
   * "drop: <kind>\t<class>\t<name>" line per dropped constraint in drop order
   * (all FKs, then uniques, then PKs). Tab, not space: a CUBRID identifier may
   * contain a space (unloaddb emits such names bracketed), and the v1 space form
   * split "order detail" into class "order" / constraint "detail" - a restored
   * rebuild input that silently re-added nothing for that class. This is
   * the Gap-R resume basis and the WU-32 rebuild input. Written at the STRIPPED
   * phase once the constraints have been dropped.
   */
  void
  render_strip_section (std::ostringstream &os, const std::vector<cubimport::stripped_constraint> &stripped)
  {
    size_t fk = 0, unique = 0, pk = 0;
    for (const cubimport::stripped_constraint &c : stripped)
      {
	switch (c.kind)
	  {
	  case cubimport::stripped_kind::FK:
	    fk++;
	    break;
	  case cubimport::stripped_kind::UNIQUE:
	    unique++;
	    break;
	  case cubimport::stripped_kind::PK:
	    pk++;
	    break;
	  }
      }

    os << "[strip]\n";
    os << "stripped: " << stripped.size () << "\n";
    os << "fk: " << fk << "\n";
    os << "unique: " << unique << "\n";
    os << "pk: " << pk << "\n";
    for (const cubimport::stripped_constraint &c : stripped)
      {
	os << "drop: " << stripped_kind_name (c.kind) << "\t" << c.cls << "\t" << c.name << "\n";
      }
    os << "\n";
  }

  /*
   * Append the [load] section from the Load phase (WU-31): the total rows
   * loaded, the count of object files loaded, and one "file: <basename> <rows>
   * <failed>" line per object file in load order. For a PER_CLASS dump each line
   * is one class; for a SINGLE dump the one line is the whole dump's total.
   * Written at the LOADED phase once the data has been loaded into bare heaps.
   */
  void
  render_load_section (std::ostringstream &os, const cubimport::load_summary &load)
  {
    os << "[load]\n";
    os << "loaded_files: " << load.loaded_files << "\n";
    os << "total_rows: " << load.total_rows << "\n";
    os << "total_failed: " << load.total_failed << "\n";
    for (const cubimport::load_file_result &f : load.files)
      {
	os << "file: " << f.object_file << " " << f.rows << " " << f.failed << "\n";
      }
    os << "\n";
  }

  /*
   * Append the [rebuild] section from the Rebuild phase (WU-32): the count of
   * re-added PK/UNIQUE constraints, pending (failed) constraints, plain indexes
   * built, and withheld FK edges, then one record line per item -
   *   ok: <kind>\t<class>\t<name>                    (rebuilt PK/UNIQUE)
   *   fail: <kind>\t<class>\t<name>\t<reason>         (pending-rebuild, exit-non-zero signal)
   *   fail_readd: <ddl>                              (its exact re-add DDL, or (none)
   *                                                  when the dump held no statement for it)
   *   index: <name>                                  (deferred plain index built)
   *   index_fail: <name>\t<reason>                    (deferred plain index that did NOT build)
   *   withhold: <child>\t<parent>\t<fk>\t<reason>      (FK cascade-withheld from WU-34)
   * Fields are TAB-delimited from format v2 (see MANIFEST_FORMAT_VERSION).
   * fail_readd is what an operator re-runs after repairing the duplicate rows,
   * and index_fail is what makes a resumed run's verdict match an uninterrupted
   * one: a failed index sets the phase PARTIAL, so a resume that could not see
   * it re-derived a clean OK and dropped the non-zero exit.
   * The pending + withhold lines are what WU-34 honors (define no FK whose parent
   * key is un-rebuilt) and what WU-50 resumes from. Written at the REBUILT phase.
   */
  void
  render_rebuild_section (std::ostringstream &os, const cubimport::rebuild_summary &rebuild)
  {
    os << "[rebuild]\n";
    os << "rebuilt: " << rebuild.rebuilt.size () << "\n";
    os << "pending: " << rebuild.pending.size () << "\n";
    os << "indexes: " << rebuild.indexes.size () << "\n";
    os << "indexes_failed: " << rebuild.failed_indexes.size () << "\n";
    os << "withheld_fk: " << rebuild.withheld.size () << "\n";
    for (const cubimport::rebuilt_constraint &c : rebuild.rebuilt)
      {
	os << "ok: " << stripped_kind_name (c.kind) << "\t" << c.cls << "\t" << c.name << "\n";
      }
    for (const cubimport::pending_rebuild &c : rebuild.pending)
      {
	os << "fail: " << stripped_kind_name (c.kind) << "\t" << c.cls << "\t" << c.name << "\t" << c.reason << "\n";
	os << "fail_readd: " << (c.readd_ddl.empty () ? NONE_MARKER : c.readd_ddl) << "\n";
      }
    for (const std::string &idx : rebuild.indexes)
      {
	os << "index: " << idx << "\n";
      }
    for (const cubimport::failed_index &f : rebuild.failed_indexes)
      {
	os << "index_fail: " << f.name << "\t" << f.reason << "\n";
      }
    for (const cubimport::withheld_fk &w : rebuild.withheld)
      {
	os << "withhold: " << w.child << "\t" << w.parent << "\t" << w.name << "\t" << w.reason << "\n";
      }
    os << "\n";
  }

  /*
   * Append the [validate] section from the FK re-validation phase (WU-33): the
   * edge tallies (validated / skipped / violated) and total orphan count, the
   * exceptions artifact basename on a violation, then one line per recorded edge
   *   edge: <child> -> <parent> (<fk>) :: clean
   *   edge: <child> -> <parent> (<fk>) :: violated <n>
   *   edge: <child> -> <parent> (<fk>) :: skipped (parent key withheld)
   * Under fail-fast the edges after the first violated one are unvalidated and
   * not listed. Written at the VALIDATED phase once FK re-validation has run.
   */
  void
  render_validate_section (std::ostringstream &os, const cubimport::validate_summary &validate)
  {
    os << "[validate]\n";
    os << "edges: " << validate.edges.size () << "\n";
    os << "validated: " << validate.validated_edges << "\n";
    os << "skipped: " << validate.skipped_edges << "\n";
    os << "violated: " << validate.violated_edges << "\n";
    os << "orphans_total: " << validate.total_orphans << "\n";
    os << "exceptions_file: " << (validate.exceptions_file.empty () ? NONE_MARKER : validate.exceptions_file) << "\n";
    for (const cubimport::fk_edge_result &r : validate.edges)
      {
	os << "edge: " << r.child << " -> " << r.parent << " (" << r.name << ") :: ";
	if (r.skipped)
	  {
	    os << "skipped (parent key withheld)";
	  }
	else if (r.orphans > 0)
	  {
	    os << "violated " << r.orphans;
	  }
	else
	  {
	    os << "clean";
	  }
	os << "\n";
      }
    os << "\n";
  }

  /*
   * Append the [fkdefine] section from the FK define phase (WU-34): the count of
   * FKs defined on the validated-clean edges and the count withheld, the
   * exceptions artifact basename when any withheld FK's re-add DDL was recorded,
   * then one record line per item -
   *   define: <child> -> <parent> (<fk>)                     (FK defined; round-trip restored)
   *   withhold: <child> -> <parent> (<fk>) :: <reason>       (FK left undefined)
   *   readd: <ALTER CLASS ... ADD CONSTRAINT ... FOREIGN KEY ...;>   (the operator's repair DDL)
   * An all-defined run means catalog == snapshot (the full round-trip); the
   * withhold + readd lines are what an operator adds after repairing the data and
   * what WU-50 resumes from. Written at the FK_DEFINED phase.
   */
  void
  render_fkdefine_section (std::ostringstream &os, const cubimport::fkdefine_summary &fkdefine)
  {
    os << "[fkdefine]\n";
    os << "defined: " << fkdefine.defined.size () << "\n";
    os << "withheld: " << fkdefine.withheld.size () << "\n";
    os << "exceptions_file: " << (fkdefine.exceptions_file.empty () ? NONE_MARKER : fkdefine.exceptions_file) << "\n";
    for (const cubimport::defined_fk &d : fkdefine.defined)
      {
	os << "define: " << d.child << " -> " << d.parent << " (" << d.name << ")\n";
      }
    for (const cubimport::withheld_define &w : fkdefine.withheld)
      {
	os << "withhold: " << w.child << " -> " << w.parent << " (" << w.name << ") :: " << w.reason << "\n";
	os << "readd: " << w.readd_ddl << "\n";
      }
    os << "\n";
  }

  /*
   * Append the [stats] section from the Statistics phase (WU-35): the count of
   * classes whose statistics refreshed and the count that failed, then one
   * "fail: <class> :: <reason>" line per class that failed. Statistics are not
   * part of the data's correctness, so a failure is recorded (the run exits
   * non-zero) but never rolls the import back. Written at the STATS_UPDATED phase.
   */
  void
  render_stats_section (std::ostringstream &os, const cubimport::stats_summary &stats)
  {
    os << "[stats]\n";
    os << "updated: " << stats.updated << "\n";
    os << "failed: " << stats.failed.size () << "\n";
    for (const cubimport::stats_failure &f : stats.failed)
      {
	os << "fail: " << f.cls << " :: " << f.reason << "\n";
      }
    os << "\n";
  }

  /*
   * Append the [triggers] section from the Trigger define phase (WU-35, the
   * terminal task): the trigger file basename (or (none) when the dump carries
   * no triggers) and the count of trigger statements defined and failed. A
   * failed statement is recorded (the run exits non-zero) but never rolls the
   * committed import back. Written at the DONE phase.
   */
  void
  render_triggers_section (std::ostringstream &os, const cubimport::trigger_summary &triggers)
  {
    os << "[triggers]\n";
    os << "trigger_file: " << (triggers.trigger_file.empty () ? NONE_MARKER : triggers.trigger_file) << "\n";
    os << "defined: " << triggers.defined << "\n";
    os << "failed: " << triggers.failed << "\n";
    os << "\n";
  }

  /* Render the manifest text for iset, marking phases up to reached complete.
   * When graph is non-null, a [graph] section is emitted before the phases; when
   * sched is non-null the [plan] section carries the real order + task counts;
   * when stripped is non-null a [strip] section lists the dropped constraints;
   * when load is non-null a [load] section records the rows loaded; when rebuild
   * is non-null a [rebuild] section records the re-added/pending constraints,
   * built indexes, and withheld FK edges. */
  std::string
  render_manifest (const cubimport::import_set &iset, cubimport::import_phase reached,
		   const cubimport::dependency_graph *graph, const cubimport::schedule *sched,
		   const std::vector<cubimport::stripped_constraint> *stripped, const cubimport::load_summary *load,
		   const cubimport::rebuild_summary *rebuild, const cubimport::validate_summary *validate,
		   const cubimport::fkdefine_summary *fkdefine, const cubimport::stats_summary *stats,
		   const cubimport::trigger_summary *triggers)
  {
    std::ostringstream os;

    os << "# CUBRID importdb manifest (format v" << MANIFEST_FORMAT_VERSION << ")\n";
    os << "# Written and owned by importdb; do not edit by hand.\n";
    os << "# Plan/progress record for the importdb directory contract (foundation N54 §4.4).\n";
    os << "format_version: " << MANIFEST_FORMAT_VERSION << "\n";
    os << "generator: importdb\n";
    os << "created: " << utc_timestamp () << "\n";
    os << "\n";

    /* [set] - the rostered ImportSet (target DB + dump layout). */
    os << "[set]\n";
    os << "database: " << iset.database_name << "\n";
    os << "dump_dir: " << iset.dump_dir << "\n";
    os << "prefix: " << iset.prefix << "\n";
    if (iset.schema_kind == cubimport::schema_layout::SPLIT)
      {
	os << "schema_layout: split\n";
	os << "schema_class_file: " << iset.schema_class_file << "\n";
	os << "schema_info_file: " << iset.schema_info_file << "\n";
	os << "schema_apply_order: " << join_list (iset.schema_apply_order) << "\n";
      }
    else
      {
	os << "schema_layout: default\n";
	os << "schema_file: " << iset.schema_file << "\n";
      }
    os << "index_file: " << (iset.index_file.empty () ? NONE_MARKER : iset.index_file) << "\n";
    os << "trigger_file: " << (iset.trigger_file.empty () ? NONE_MARKER : iset.trigger_file) << "\n";
    os << "object_layout: " << (iset.object_kind == cubimport::object_layout::PER_CLASS ? "per-class" : "single") << "\n";
    os << "object_count: " << iset.object_files.size () << "\n";
    os << "object_files: " << join_list (iset.object_files) << "\n";
    os << "\n";

    /* [plan] - the plan record. Once the Planner has run (WU-22) this is the
     * real data-level order + terminal task counts; before then it is an
     * explicit placeholder, not a fabricated order. */
    if (sched != NULL)
      {
	render_plan_section (os, *sched);
      }
    else
      {
	os << "[plan]\n";
	os << "planned_order: " << PLANNED_ORDER_PLACEHOLDER << "\n";
	os << "\n";
      }

    /* [graph] - the DependencyGraph snapshot (WU-21), present once the Graph
     * builder has run (DEFINED phase). */
    if (graph != NULL)
      {
	render_graph_section (os, *graph);
      }

    /* [strip] - the dropped constraints (WU-30), present once the Strip phase
     * has run (STRIPPED phase). */
    if (stripped != NULL)
      {
	render_strip_section (os, *stripped);
      }

    /* [load] - the rows loaded (WU-31), present once the Load phase has run
     * (LOADED phase). */
    if (load != NULL)
      {
	render_load_section (os, *load);
      }

    /* [rebuild] - the re-added PK/UNIQUE + built indexes, plus the pending and
     * withheld sets (WU-32), present once the Rebuild phase has run (REBUILT
     * phase). */
    if (rebuild != NULL)
      {
	render_rebuild_section (os, *rebuild);
      }

    /* [validate] - the FK re-validation outcome (WU-33), present once the FK
     * re-validation phase has run (VALIDATED phase). */
    if (validate != NULL)
      {
	render_validate_section (os, *validate);
      }

    /* [fkdefine] - the FK define outcome (WU-34), present once the FK define
     * phase has run (FK_DEFINED phase). */
    if (fkdefine != NULL)
      {
	render_fkdefine_section (os, *fkdefine);
      }

    /* [stats] - the per-class statistics refresh (WU-35), present once the
     * Statistics phase has run (STATS_UPDATED phase). */
    if (stats != NULL)
      {
	render_stats_section (os, *stats);
      }

    /* [triggers] - the deferred trigger define (WU-35, terminal task), present
     * once the Trigger define phase has run (DONE phase). */
    if (triggers != NULL)
      {
	render_triggers_section (os, *triggers);
      }

    /* [phases] - the run/phase state. Every phase up to and including the
     * furthest reached is complete; the rest are pending and advance in later
     * WUs. "current" names the furthest-reached phase so resume (WU-50) can
     * pick up. */
    const int reached_idx = (int) reached;
    os << "[phases]\n";
    os << "current: " << PHASE_NAMES[reached_idx] << "\n";
    for (int i = 0; i < (int) (sizeof (PHASE_NAMES) / sizeof (PHASE_NAMES[0])); i++)
      {
	os << PHASE_NAMES[i] << ": " << (i <= reached_idx ? "complete" : "pending") << "\n";
      }

    return os.str ();
  }

  /* Write content to final_path crash-safely: temp file + fsync + atomic
   * rename. On failure preserves the original errno for the caller's
   * diagnostic and leaves no partial manifest at final_path. */
  bool
  atomic_write (const std::string &final_path, const std::string &content)
  {
    const std::string tmp_path = final_path + ".tmp." + std::to_string ((long) getpid ());

    int fd = open (tmp_path.c_str (), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
      {
	return false;
      }

    const char *buf = content.data ();
    size_t remaining = content.size ();
    while (remaining > 0)
      {
	ssize_t n = write (fd, buf, remaining);
	if (n < 0)
	  {
	    if (errno == EINTR)
	      {
		continue;
	      }
	    int saved = errno;
	    close (fd);
	    unlink (tmp_path.c_str ());
	    errno = saved;
	    return false;
	  }
	buf += n;
	remaining -= (size_t) n;
      }

    if (fsync (fd) != 0)
      {
	int saved = errno;
	close (fd);
	unlink (tmp_path.c_str ());
	errno = saved;
	return false;
      }
    if (close (fd) != 0)
      {
	int saved = errno;
	unlink (tmp_path.c_str ());
	errno = saved;
	return false;
      }

    if (rename (tmp_path.c_str (), final_path.c_str ()) != 0)
      {
	int saved = errno;
	unlink (tmp_path.c_str ());
	errno = saved;
	return false;
      }

    /* fsync the directory so the rename itself is durable across a crash. A
     * failure here does not leave a partial manifest, so it is non-fatal. */
    std::string dir = final_path;
    const size_t slash = dir.find_last_of ('/');
    dir = (slash == std::string::npos) ? std::string (".") : dir.substr (0, slash == 0 ? 1 : slash);
    int dfd = open (dir.c_str (), O_RDONLY);
    if (dfd >= 0)
      {
	fsync (dfd);
	close (dfd);
      }

    return true;
  }

  /* ---- WU-50 manifest reader helpers (parse render_*_section output) ---- */

  std::string
  trim (const std::string &s)
  {
    const size_t a = s.find_first_not_of (" \t\r\n");
    if (a == std::string::npos)
      {
	return "";
      }
    const size_t b = s.find_last_not_of (" \t\r\n");
    return s.substr (a, b - a + 1);
  }

  std::vector<std::string>
  split_char (const std::string &s, char d)
  {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
      {
	if (c == d)
	  {
	    out.push_back (cur);
	    cur.clear ();
	  }
	else
	  {
	    cur += c;
	  }
      }
    out.push_back (cur);
    return out;
  }

  /*
   * Split one record line into its fields. Format v2 writes them tab-delimited,
   * because a CUBRID identifier may contain a space. v1 wrote them
   * space-delimited with the reason (when there is one) after " :: "; that form
   * is still accepted so a manifest written before the bump still reads, and it
   * is still wrong for an identifier with a space - which is exactly why v2
   * exists. Returns false when fewer than `least` fields came out.
   *
   * The v2/v1 decision is made by SNIFFING for a tab rather than by reading
   * format_version, which is safe only because of a property every record type
   * happens to have today: each carries at least two fields that cannot both be
   * empty, so at least one tab always survives (parse_kv trims the value, and
   * trim strips tabs, so a trailing EMPTY field loses its tab). The two branches
   * are not equivalent - v1 drops empty fields, v2 keeps them - so a record type
   * added later whose field count equals its `least` AND whose last field can be
   * empty would silently reparse as v1 and come up short. Add such a record and
   * this must key on format_version instead.
   */
  bool
  split_record (const std::string &val, size_t least, std::vector<std::string> &out)
  {
    out.clear ();
    if (val.find ('\t') != std::string::npos)
      {
	for (const std::string &f : split_char (val, '\t'))
	  {
	    out.push_back (trim (f));
	  }
	return out.size () >= least;
      }

    /* v1 */
    const size_t sep = val.find (" :: ");
    const std::string left = (sep == std::string::npos) ? trim (val) : trim (val.substr (0, sep));
    for (const std::string &f : split_char (left, ' '))
      {
	if (!trim (f).empty ())
	  {
	    out.push_back (trim (f));
	  }
      }
    if (sep != std::string::npos)
      {
	out.push_back (trim (val.substr (sep + 4)));
      }
    return out.size () >= least;
  }

  /* Parse a "a, b, c" join_list() value into a vector; "(none)"/"" -> empty. */
  std::vector<std::string>
  parse_list (const std::string &s)
  {
    std::vector<std::string> out;
    const std::string t = trim (s);
    if (t.empty () || t == NONE_MARKER)
      {
	return out;
      }
    for (const std::string &part : split_char (t, ','))
      {
	const std::string v = trim (part);
	if (!v.empty ())
	  {
	    out.push_back (v);
	  }
      }
    return out;
  }

  /* Split "key: value" -> key (trimmed), value (trimmed on the left only so a
   * tab-delimited detail value keeps its internal tabs). false if no ':'. */
  bool
  parse_kv (const std::string &line, std::string &key, std::string &val)
  {
    const size_t c = line.find (':');
    if (c == std::string::npos)
      {
	return false;
      }
    key = trim (line.substr (0, c));
    val = trim (line.substr (c + 1));
    return true;
  }

  bool
  phase_from_name (const std::string &name, cubimport::import_phase &out)
  {
    for (int i = 0; i < (int) (sizeof (PHASE_NAMES) / sizeof (PHASE_NAMES[0])); i++)
      {
	if (name == PHASE_NAMES[i])
	  {
	    out = (cubimport::import_phase) i;
	    return true;
	  }
      }
    return false;
  }

  bool
  kind_from_name (const std::string &name, cubimport::stripped_kind &out)
  {
    if (name == "fk")
      {
	out = cubimport::stripped_kind::FK;
	return true;
      }
    if (name == "unique")
      {
	out = cubimport::stripped_kind::UNIQUE;
	return true;
      }
    if (name == "pk")
      {
	out = cubimport::stripped_kind::PK;
	return true;
      }
    return false;
  }

  /* Value after "<key>=" for the first tab-field (from index `from`) that starts
   * with "<key>=", or "" if none. */
  std::string
  field_val (const std::vector<std::string> &fields, size_t from, const char *key)
  {
    const std::string pfx = std::string (key) + "=";
    for (size_t i = from; i < fields.size (); i++)
      {
	if (fields[i].compare (0, pfx.size (), pfx) == 0)
	  {
	    return fields[i].substr (pfx.size ());
	  }
      }
    return "";
  }

  /* "gnode:" value: "<name>\tcs=..\tpart=..\tpk=..\tuk=..\tfk=..\tpartn=..\tser=.." */
  void
  parse_gnode (const std::string &val, cubimport::dependency_graph &g)
  {
    const std::vector<std::string> f = split_char (val, '\t');
    if (f.empty () || trim (f[0]).empty ())
      {
	return;
      }
    cubimport::graph_node n;
    n.name = trim (f[0]);
    n.cs_loadable = (field_val (f, 1, "cs") != "0");
    n.partitioned = (field_val (f, 1, "part") == "1");
    n.constraints.pk = field_val (f, 1, "pk");
    n.constraints.unique = parse_list (field_val (f, 1, "uk"));
    n.constraints.fk = parse_list (field_val (f, 1, "fk"));
    n.partitions = parse_list (field_val (f, 1, "partn"));
    n.serials = parse_list (field_val (f, 1, "ser"));
    g.nodes.push_back (n);
  }

  /* "gfk:" value: "<child>\t<parent>\t<name>\tcc=..\tppk=..\tcpk=.." */
  void
  parse_gfk (const std::string &val, cubimport::dependency_graph &g)
  {
    const std::vector<std::string> f = split_char (val, '\t');
    if (f.size () < 3)
      {
	return;
      }
    cubimport::fk_edge e;
    e.child = trim (f[0]);
    e.parent = trim (f[1]);
    e.name = trim (f[2]);
    e.child_columns = parse_list (field_val (f, 3, "cc"));
    e.parent_pk_columns = parse_list (field_val (f, 3, "ppk"));
    e.child_pk_columns = parse_list (field_val (f, 3, "cpk"));
    g.fk_edges.push_back (e);
  }

  /* "ginh:" value: "<child>\t<super>" */
  void
  parse_ginh (const std::string &val, cubimport::dependency_graph &g)
  {
    const std::vector<std::string> f = split_char (val, '\t');
    if (f.size () < 2)
      {
	return;
      }
    cubimport::inherit_edge e;
    e.child = trim (f[0]);
    e.super = trim (f[1]);
    g.inherit_edges.push_back (e);
  }

  /* "drop:" value: "<kind> <cls> <name>" (the [strip] section, in drop order). */
  void
  parse_drop (const std::string &val, std::vector<cubimport::stripped_constraint> &out)
  {
    std::vector<std::string> f;
    cubimport::stripped_kind k;
    if (!split_record (val, 3, f) || !kind_from_name (f[0], k))
      {
	return;
      }
    cubimport::stripped_constraint c;
    c.kind = k;
    c.cls = f[1];
    c.name = f[2];
    out.push_back (c);
  }

  /* Parse the render convention for an FK edge reference,
   * "<child> -> <parent> (<fk name>)", into its three parts. A malformed
   * reference leaves the parts it could not find empty. */
  void
  parse_edge_ref (const std::string &ref, std::string &child, std::string &parent, std::string &name)
  {
    child.clear ();
    parent.clear ();
    name.clear ();

    const size_t arrow = ref.find (" -> ");
    if (arrow == std::string::npos)
      {
	return;
      }
    child = trim (ref.substr (0, arrow));

    const std::string rest = trim (ref.substr (arrow + 4));	/* "<parent> (<name>)" */
    const size_t paren = rest.find (" (");
    if (paren == std::string::npos)
      {
	parent = rest;
	return;
      }
    parent = trim (rest.substr (0, paren));
    std::string nm = rest.substr (paren + 2);
    const size_t rp = nm.find (')');
    if (rp != std::string::npos)
      {
	nm = nm.substr (0, rp);
      }
    name = trim (nm);
  }

  /* left/right split on the first " :: " separator (the render convention). */
  void
  split_dblcolon (const std::string &val, std::string &left, std::string &right)
  {
    const size_t p = val.find (" :: ");
    if (p == std::string::npos)
      {
	left = trim (val);
	right = "";
	return;
      }
    left = trim (val.substr (0, p));
    right = trim (val.substr (p + 4));
  }
} // namespace

namespace cubimport
{

  bool
  write_manifest (const import_set &iset, import_phase reached, const dependency_graph *graph, const schedule *sched,
		  const std::vector<stripped_constraint> *stripped, const load_summary *load,
		  const rebuild_summary *rebuild, const validate_summary *validate, const fkdefine_summary *fkdefine,
		  const stats_summary *stats, const trigger_summary *triggers)
  {
    const std::string path = path_join (iset.dump_dir, MANIFEST_BASENAME);
    const std::string content = render_manifest (iset, reached, graph, sched, stripped, load, rebuild, validate,
				fkdefine, stats, triggers);

    if (!atomic_write (path, content))
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_MANIFEST_WRITE_FAILED), path.c_str (), strerror (errno));
	return false;
      }
    return true;
  }

  const char *
  phase_name (import_phase phase)
  {
    const int idx = (int) phase;
    if (idx < 0 || idx >= (int) (sizeof (PHASE_NAMES) / sizeof (PHASE_NAMES[0])))
      {
	return "unknown";
      }
    return PHASE_NAMES[idx];
  }

  int
  manifest_format_version ()
  {
    return MANIFEST_FORMAT_VERSION;
  }

  bool
  preserve_manifest (const std::string &dump_dir, std::string &saved_path)
  {
    const std::string path = path_join (dump_dir, MANIFEST_BASENAME);
    struct stat sb;
    if (stat (path.c_str (), &sb) != 0)
      {
	return false;		/* nothing to preserve */
      }

    /* Never overwrite an existing .prev. The refusal path also writes a fresh
     * DISCOVERED stub, so a SECOND refusal would otherwise rename that worthless
     * stub over the real backup - destroying the record on the very attempt that
     * was trying to recover it. Fall back to .prev.1, .prev.2, ... */
    std::string prev = path + ".prev";
    for (int i = 1; i <= 99; i++)
      {
	if (stat (prev.c_str (), &sb) != 0)
	  {
	    break;
	  }
	prev = path + ".prev." + std::to_string (i);
      }

    if (rename (path.c_str (), prev.c_str ()) != 0)
      {
	return false;
      }
    saved_path = prev;
    return true;
  }

  manifest_read
  read_manifest (const std::string &dump_dir, manifest_state &st, std::string &error)
  {
    const std::string path = path_join (dump_dir, MANIFEST_BASENAME);

    /* ENOENT is "fresh run"; anything else is a manifest that exists and cannot
     * be read, which is a different situation entirely - see manifest_read. */
    struct stat sb;
    if (stat (path.c_str (), &sb) != 0)
      {
	if (errno != ENOENT)
	  {
	    error = strerror (errno);
	    return manifest_read::UNREADABLE;
	  }
	return manifest_read::ABSENT;
      }

    std::ifstream in (path.c_str ());
    if (!in.is_open ())
      {
	error = strerror (errno);
	return manifest_read::UNREADABLE;
      }
    /* Retained for callers that only want "was there a manifest?"; the
     * resume decision switches on the manifest_read result instead. */
    st.present = true;

    std::string section;
    std::string raw;
    while (std::getline (in, raw))
      {
	if (!raw.empty () && raw.back () == '\r')
	  {
	    raw.pop_back ();
	  }
	const std::string t = trim (raw);
	if (t.empty () || t[0] == '#')
	  {
	    continue;
	  }
	if (t[0] == '[')
	  {
	    section = t;
	    if (section == "[strip]")
	      {
		st.has_strip = true;
	      }
	    else if (section == "[rebuild]")
	      {
		st.has_rebuild = true;
	      }
	    else if (section == "[validate]")
	      {
		st.has_validate = true;
	      }
	    else if (section == "[load]")
	      {
		st.has_load = true;
	      }
	    else if (section == "[fkdefine]")
	      {
		st.has_fkdefine = true;
	      }
	    else if (section == "[stats]")
	      {
		st.has_stats = true;
	      }
	    else if (section == "[triggers]")
	      {
		st.has_triggers = true;
	      }
	    else if (section == "[graph]")
	      {
		/* On the header, as the others are: a dump with no user classes
		 * writes the section with no gnode: lines at all, and keying the
		 * flag off a node would refuse to resume it. */
		st.has_graph = true;
	      }
	    continue;
	  }

	std::string key, val;
	if (!parse_kv (raw, key, val))
	  {
	    continue;
	  }

	if (section.empty ())
	  {
	    if (key == "format_version")
	      {
		st.format_version = atoi (val.c_str ());
	      }
	    continue;
	  }

	if (section == "[set]")
	  {
	    if (key == "database")
	      {
		st.database = val;
	      }
	    else if (key == "dump_dir")
	      {
		st.dump_dir = val;
	      }
	    else if (key == "prefix")
	      {
		st.prefix = val;
	      }
	  }
	else if (section == "[graph]")
	  {
	    if (key == "gnode")
	      {
		parse_gnode (val, st.graph);
	      }
	    else if (key == "gfk")
	      {
		parse_gfk (val, st.graph);
	      }
	    else if (key == "ginh")
	      {
		parse_ginh (val, st.graph);
	      }
	    else if (key == "cycle")
	      {
		/* "a -> b -> a" - the node list join_cycle () wrote. */
		std::vector<std::string> cy;
		size_t from = 0;
		for (size_t arrow = val.find (" -> "); ; arrow = val.find (" -> ", from))
		  {
		    if (arrow == std::string::npos)
		      {
			cy.push_back (trim (val.substr (from)));
			break;
		      }
		    cy.push_back (trim (val.substr (from, arrow - from)));
		    from = arrow + 4;
		  }
		st.graph.cycles.push_back (cy);
	      }
	    else if (key == "skipped_classes")
	      {
		st.graph.skipped_classes = parse_list (val);
	      }
	  }
	else if (section == "[plan]")
	  {
	    if (key == "planned_order")
	      {
		/* "a, b | c, d" - the Planner's data-phase level sets. They live
		 * on the graph (build_schedule copies them into the Schedule), so
		 * a resumed run that did not run the Graph builder needs them
		 * back from here; otherwise its schedule would report an empty
		 * data phase and rewrite the [plan] section as one. */
		/* A manifest written between the Graph builder and the Planner
		 * still carries the WU-12 placeholder here; it is prose, not an
		 * order, and must not parse as a one-class level. */
		if (val != NONE_MARKER && val != PLANNED_ORDER_PLACEHOLDER)
		  {
		    for (const std::string &lvl : split_char (val, '|'))
		      {
			const std::vector<std::string> classes = parse_list (lvl);
			if (!classes.empty ())
			  {
			    st.graph.level_sets.push_back (classes);
			  }
		      }
		  }
	      }
	  }
	else if (section == "[strip]")
	  {
	    if (key == "drop")
	      {
		parse_drop (val, st.stripped);
	      }
	  }
	else if (section == "[rebuild]")
	  {
	    std::vector<std::string> f;
	    if (key == "ok")
	      {
		stripped_kind k;
		if (split_record (val, 3, f) && kind_from_name (f[0], k))
		  {
		    rebuilt_constraint c;
		    c.kind = k;
		    c.cls = f[1];
		    c.name = f[2];
		    st.rebuild.rebuilt.push_back (c);
		  }
	      }
	    else if (key == "fail")
	      {
		stripped_kind k;
		if (split_record (val, 3, f) && kind_from_name (f[0], k))
		  {
		    pending_rebuild c;
		    c.kind = k;
		    c.cls = f[1];
		    c.name = f[2];
		    c.reason = (f.size () > 3) ? f[3] : "";
		    st.rebuild.pending.push_back (c);
		  }
	      }
	    else if (key == "fail_readd")
	      {
		/* Always follows its own fail: line - the operator's exact repair
		 * statement, which before v2 lived only in memory and came back
		 * empty on a resumed report. */
		if (!st.rebuild.pending.empty ())
		  {
		    st.rebuild.pending.back ().readd_ddl = val;
		  }
	      }
	    else if (key == "index")
	      {
		st.rebuild.indexes.push_back (trim (val));
	      }
	    else if (key == "index_fail")
	      {
		if (split_record (val, 1, f))
		  {
		    failed_index fi;
		    fi.name = f[0];
		    fi.reason = (f.size () > 1) ? f[1] : "";
		    st.rebuild.failed_indexes.push_back (fi);
		  }
	      }
	    else if (key == "withhold")
	      {
		if (split_record (val, 3, f))
		  {
		    withheld_fk w;
		    w.child = f[0];
		    w.parent = f[1];
		    w.name = f[2];
		    w.reason = (f.size () > 3) ? f[3] : "";
		    st.rebuild.withheld.push_back (w);
		  }
	      }
	  }
	else if (section == "[validate]")
	  {
	    if (key == "exceptions_file")
	      {
		if (val != NONE_MARKER)
		  {
		    st.validate.exceptions_file = val;
		  }
	      }
	    else if (key == "edge")
	      {
		std::string left, status;
		split_dblcolon (val, left, status);
		fk_edge_result r;
		parse_edge_ref (left, r.child, r.parent, r.name);
		if (status.compare (0, 7, "skipped") == 0)
		  {
		    r.skipped = true;
		    st.validate.skipped_edges++;
		  }
		else if (status.compare (0, 8, "violated") == 0)
		  {
		    r.orphans = atoll (status.c_str () + 8);
		    st.validate.violated_edges++;
		    st.validate.total_orphans += r.orphans;
		  }
		else
		  {
		    st.validate.validated_edges++;
		  }
		st.validate.edges.push_back (r);
	      }
	  }
	else if (section == "[load]")
	  {
	    if (key == "loaded_files")
	      {
		st.load.loaded_files = atoi (val.c_str ());
	      }
	    else if (key == "total_rows")
	      {
		st.load.total_rows = atoll (val.c_str ());
	      }
	    else if (key == "total_failed")
	      {
		st.load.total_failed = atoll (val.c_str ());
	      }
	    else if (key == "file")
	      {
		/* "<basename> <rows> <failed>" - the counts are the last two
		 * space-separated fields, whatever the basename holds. */
		const std::vector<std::string> f = split_char (trim (val), ' ');
		if (f.size () >= 3)
		  {
		    load_file_result r;
		    r.object_file = f[f.size () - 3];
		    r.rows = atoll (f[f.size () - 2].c_str ());
		    r.failed = atoll (f[f.size () - 1].c_str ());
		    st.load.files.push_back (r);
		  }
	      }
	  }
	else if (section == "[fkdefine]")
	  {
	    if (key == "exceptions_file")
	      {
		if (val != NONE_MARKER)
		  {
		    st.fkdefine.exceptions_file = val;
		  }
	      }
	    else if (key == "define")
	      {
		defined_fk d;
		parse_edge_ref (trim (val), d.child, d.parent, d.name);
		st.fkdefine.defined.push_back (d);
	      }
	    else if (key == "withhold")
	      {
		std::string left, reason;
		split_dblcolon (val, left, reason);
		withheld_define w;
		parse_edge_ref (left, w.child, w.parent, w.name);
		w.reason = reason;
		st.fkdefine.withheld.push_back (w);
	      }
	    else if (key == "readd")
	      {
		/* The re-add DDL always follows its own withhold line. */
		if (!st.fkdefine.withheld.empty ())
		  {
		    st.fkdefine.withheld.back ().readd_ddl = val;
		  }
	      }
	  }
	else if (section == "[stats]")
	  {
	    if (key == "updated")
	      {
		st.stats.updated = atoi (val.c_str ());
	      }
	    else if (key == "fail")
	      {
		std::string cls, reason;
		split_dblcolon (val, cls, reason);
		st.stats.failed.push_back ({ cls, reason });
	      }
	  }
	else if (section == "[triggers]")
	  {
	    if (key == "trigger_file")
	      {
		if (val != NONE_MARKER)
		  {
		    st.triggers.trigger_file = val;
		  }
	      }
	    else if (key == "defined")
	      {
		st.triggers.defined = atoi (val.c_str ());
	      }
	    else if (key == "failed")
	      {
		st.triggers.failed = atoi (val.c_str ());
	      }
	  }
	else if (section == "[phases]")
	  {
	    if (key == "current")
	      {
		import_phase p;
		if (phase_from_name (val, p))
		  {
		    st.reached = p;
		    st.has_phase = true;
		  }
	      }
	  }
      }

    st.graph.database_name = st.database;
    return manifest_read::OK;
  }

} // namespace cubimport
