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
 * import_manifest.hpp - The importdb manifest (plan/progress record), v0
 *
 * After Discovery rosters the dump into an ImportSet, importdb writes a
 * manifest file INTO the importdb directory (the dump dir it owns, foundation
 * §4.4). The manifest is importdb's plan/progress record: it captures the
 * rostered set, a plan record with a planned-order placeholder (the real
 * topological order is WU-22), and per-phase run state (only "discovered" is
 * reached at v0). Later WUs enrich it in place - the planner (WU-22) fills the
 * plan order, the executor (WU-30+) advances the phase markers, and resume
 * (WU-50) reads the per-phase state back. The write is crash-safe (temp file +
 * atomic rename), so the manifest is always parseable after any run state.
 */

#ifndef _IMPORT_MANIFEST_HPP_
#define _IMPORT_MANIFEST_HPP_

#include "import_discovery.hpp"
#include "import_graph.hpp"	/* dependency_graph, by value in manifest_state (WU-50) */
#include "import_strip.hpp"	/* stripped_constraint */
#include "import_load.hpp"	/* load_summary */
#include "import_rebuild.hpp"	/* rebuild_summary */
#include "import_validate.hpp"	/* validate_summary */
#include "import_fkdefine.hpp"	/* fkdefine_summary */
#include "import_stats.hpp"	/* stats_summary */
#include "import_triggers.hpp"	/* trigger_summary */

#include <string>
#include <vector>

namespace cubimport
{

  struct schedule;		/* import_plan.hpp - optional [plan] section source (WU-22) */

  /*
   * The furthest importdb phase reached, in pipeline order. The manifest's
   * phase block marks every phase up to and including reached as "complete"
   * and the rest "pending"; "current" names reached. WU-12 writes DISCOVERED;
   * WU-20 advances to DEFINED after a successful define; WU-30 advances to
   * STRIPPED after the constraints are dropped; WU-34 advances to FK_DEFINED
   * after the FK is defined on the validated-clean edges; WU-35 advances to
   * STATS_UPDATED after the per-class statistics refresh and to DONE after the
   * deferred triggers are defined (the terminal task).
   */
  enum class import_phase
  {
    DISCOVERED = 0,
    DEFINED,
    STRIPPED,
    LOADED,
    REBUILT,
    /* No VALIDATED: FK re-validation is not a phase any more. The engine
     * validates while it builds the FK (btree_load_check_fk), so FK_DEFINED
     * covers both -- see import_validate.cpp's header. Manifest format v3. */
    FK_DEFINED,
    STATS_UPDATED,
    DONE
  };

  /*
   * Write the importdb manifest for iset into iset.dump_dir, crash-safely
   * (temp file + rename), recording reached as the furthest phase. When graph
   * is non-null (the DEFINED phase, after the Graph builder's snapshot) a
   * [graph] section records the node/edge counts, detected cycles, and any
   * skipped (object-valued) classes. When sched is non-null (after the Planner,
   * WU-22) the [plan] section carries the real data-level order + terminal task
   * counts instead of the WU-12 placeholder. When stripped is non-null (the
   * STRIPPED phase, after the Strip phase WU-30) a [strip] section lists the
   * dropped pk/uk/fk per class in drop order - the Gap-R resume basis and the
   * WU-32 rebuild input. When load is non-null (the LOADED phase, after the Load
   * phase WU-31) a [load] section records the rows loaded per object file and in
   * total. When rebuild is non-null (the REBUILT phase, after the Rebuild phase
   * WU-32) a [rebuild] section records the re-added PK/UNIQUE, the plain indexes
   * built, the pending-rebuild set (PK/UNIQUE that failed the duplicate-key
   * contract), and the FK edges the cascade withheld from WU-34. When validate is
   * non-null (the VALIDATED phase, after the FK re-validation phase WU-33) a
   * [validate] section records each FK edge's outcome (clean / violated with an
   * orphan count / skipped when its parent PK was withheld) and the exceptions
   * artifact basename on a violation. When fkdefine is non-null (the FK_DEFINED
   * phase, after the FK define phase WU-34) a [fkdefine] section records the FKs
   * defined on the validated-clean edges and the edges withheld (with their exact
   * re-add DDL). When stats is non-null (the STATS_UPDATED phase, after the
   * statistics phase WU-35) a [stats] section records the classes whose
   * statistics refreshed and the classes that failed. When triggers is non-null
   * (the DONE phase, after the trigger define phase WU-35) a [triggers] section
   * records the trigger file and the defined/failed statement counts. Returns
   * true on success; on failure emits the named diagnostic
   * (IMPORTDB_MSG_MANIFEST_WRITE_FAILED) and returns false.
   */
  bool write_manifest (const import_set &iset, import_phase reached, const dependency_graph *graph = nullptr,
		       const schedule *sched = nullptr,
		       const std::vector<stripped_constraint> *stripped = nullptr, const load_summary *load = nullptr,
		       const rebuild_summary *rebuild = nullptr, const validate_summary *validate = nullptr,
		       const fkdefine_summary *fkdefine = nullptr, const stats_summary *stats = nullptr,
		       const trigger_summary *triggers = nullptr);

  /*
   * The state read back from a prior run's manifest - the WU-50 resume basis.
   * `present` is true iff a manifest file was found and parsed. `reached` is the
   * furthest phase that prior run completed. The `[set]` identity fields
   * (database / dump_dir / prefix) let the caller guard resume against an
   * unrelated stale manifest. The reconstructed state members are populated only
   * when their section was present (has_* flags): after STRIP the catalog is
   * bare, so `graph` is recovered here rather than rebuilt from the catalog;
   * `stripped` is the rebuild input; `rebuild` / `validate` are what the FK
   * define / re-validation phases consume when their producing phase is skipped
   * on resume. `load` / `fkdefine` / `stats` / `triggers` carry no input any
   * later phase reads, but a resumed run re-emits them into the manifest it
   * rewrites: without them a resume past those phases would replace an honest
   * record (rows loaded, FKs withheld with their re-add DDL) with zeroes.
   * The Planner's level sets are read back too (from [plan], where they are
   * written): the Schedule copies them off the graph rather than recomputing
   * them, so without them a resumed run would report - and re-record - an empty
   * data phase.
   */
  struct manifest_state
  {
    bool present = false;
    int format_version = 0;
    /* False when the file carried no parseable [phases] current: line. `reached`
     * then holds its DISCOVERED default, which is a real phase - so without this
     * flag a truncated manifest is indistinguishable from one written before the
     * definition phase finished, and the caller would give an operator the
     * "drop the partial schema" advice about a fully loaded database. */
    bool has_phase = false;
    import_phase reached = import_phase::DISCOVERED;

    std::string database;	/* [set] identity - resume guard */
    std::string dump_dir;
    std::string prefix;

    bool has_graph = false;
    dependency_graph graph;
    bool has_strip = false;
    std::vector<stripped_constraint> stripped;
    bool has_rebuild = false;
    rebuild_summary rebuild;
    bool has_validate = false;
    validate_summary validate;
    bool has_load = false;
    load_summary load;
    bool has_fkdefine = false;
    fkdefine_summary fkdefine;
    bool has_stats = false;
    stats_summary stats;
    bool has_triggers = false;
    trigger_summary triggers;
  };

  /*
   * The manifest's own name for a phase ("discovered", "defined", ...) - what
   * the [phases] block writes and what a resume message names. Never null.
   */
  const char *phase_name (import_phase phase);

  /* The on-disk format this build writes, and the highest it can read. */
  int manifest_format_version ();

  /*
   * The outcome of looking for a manifest. ABSENT and UNREADABLE must not be
   * conflated: "no manifest" means a fresh import is correct, while "a manifest
   * is there but I could not read it" means the target may already be half
   * imported and importing from scratch would define over it.
   */
  enum class manifest_read
  {
    ABSENT = 0,
    OK,
    UNREADABLE
  };

  /*
   * Read the manifest from dump_dir into st. Returns OK (st.present = true) when
   * the file exists and parses, ABSENT when there is no file, or UNREADABLE when
   * a file is there but could not be opened - with the reason in error. A parse
   * tolerant of unknown keys/sections (forward-compatible); populates only the
   * sections that are present. Does NOT connect to the server - it only reads
   * the file.
   */
  manifest_read read_manifest (const std::string &dump_dir, manifest_state &st, std::string &error);

  /*
   * Rename an existing manifest out of the way, to <name>.prev, so that a run
   * which is about to write a fresh one does not destroy the only record of an
   * interrupted import's progress. Returns true when a manifest was preserved
   * and sets saved_path to where it went; false when there was nothing to
   * preserve or the rename failed (which is not itself fatal - the caller is
   * about to overwrite either way).
   */
  bool preserve_manifest (const std::string &dump_dir, std::string &saved_path);

} // namespace cubimport

#endif /* _IMPORT_MANIFEST_HPP_ */
