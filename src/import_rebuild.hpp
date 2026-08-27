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
 * import_rebuild.hpp - Stage 4 (Rebuild phase, step 3 of the constraint lifecycle)
 *
 * After the Load phase (WU-31) fills the bare heaps, the Rebuild phase re-adds
 * the PK/UNIQUE constraints the Strip phase (WU-30) removed and builds the
 * deferred plain indexes on the now-populated tables (10-design.md §6). This is
 * the terminal-task order the Planner emits: REBUILD_PK / REBUILD_UNIQUE first,
 * then BUILD_INDEX. FK re-validate/define (WU-33/34) and stats/triggers (WU-35)
 * are NOT done here.
 *
 * The rebuild re-executes the dump's OWN PK/UK ADD statements verbatim rather
 * than synthesizing DDL: for a SPLIT dump it runs the isolated <prefix>_schema_pk
 * then <prefix>_schema_uk files (named in iset.schema_apply_order; the UK file
 * may be absent); for a DEFAULT dump it isolates, from the single <prefix>_schema
 * text, exactly the ALTER CLASS ... ADD ATTRIBUTE CONSTRAINT [<name>] PRIMARY
 * KEY|UNIQUE(...) statements whose constraint name matches the stripped PK/UK set
 * (CREATE CLASS / column / serial / FK statements are skipped - already applied
 * at define, or deferred to WU-34). The deferred plain indexes come from
 * <prefix>_indexes (iset.index_file, may be absent). Statements run one at a time
 * on the already-open session with the same public single-statement primitive the
 * Strip phase uses (db_execute), authorization disabled for the DBA-group path.
 * On a populated table an ADD CONSTRAINT bulk-builds the b-tree server-side
 * (sm_add_constraint -> btree_load_index); loaddb is not touched.
 *
 * Rebuild-failure contract (10-design.md §6, the duplicate-key path): a
 * populated-table PK/UNIQUE ADD can fail on a duplicate key (btree_load_index
 * stops at the first duplicate - fail-fast-single per constraint; there is NO
 * --continue enumeration for PK/UK). On such a failure the constraint is left
 * un-rebuilt and RECORDED in the pending-rebuild set (class + constraint + kind +
 * reason + the exact re-add DDL); every FK edge whose PARENT is that class is
 * RECORDED as withheld (so
 * WU-34 does not define it); the phase CONTINUES attempting the other rebuilds,
 * and the run ultimately exits non-zero (the caller still commits the
 * successfully-rebuilt constraints + the loaded data - a partial, honestly
 * recorded result). No FK is undone here: at WU-32 in isolation FK is not defined
 * yet, so the cascade is a manifest record WU-34 will honor. The caller owns the
 * transaction (import_session.hpp): rebuild () neither commits nor aborts.
 */

#ifndef _IMPORT_REBUILD_HPP_
#define _IMPORT_REBUILD_HPP_

#include "import_strip.hpp"

#include <string>
#include <vector>

namespace cubimport
{

  /* import_resume.hpp - the WU-50 resume guard. Only ever taken by pointer
   * here, and declaring it keeps the include graph acyclic (import_resume.hpp
   * needs this header for stripped_constraint). */
  struct catalog_state;

  /* One successfully re-added PK/UNIQUE constraint (kind is PK or UNIQUE only). */
  struct rebuilt_constraint
  {
    stripped_kind kind;
    std::string cls;
    std::string name;
  };

  /*
   * One PK/UNIQUE that failed its populated-table rebuild (typically a duplicate
   * key): the class it was on, its kind, the constraint name, the server's
   * failure reason, and the exact re-add DDL - the dump's own verbatim PK/UK ADD
   * statement an operator re-runs to restore the constraint after repairing the
   * offending rows (mirrors withheld_define.readd_ddl on the FK side). The
   * pending-rebuild set is the WU-50 resume basis and the signal that the run
   * exits non-zero.
   */
  struct pending_rebuild
  {
    stripped_kind kind;
    std::string cls;
    std::string name;
    std::string reason;
    std::string readd_ddl;
  };

  /*
   * One FK edge withheld by the cascade: its PARENT class had a PK/UNIQUE fail to
   * rebuild, so the edge's FK must not be defined in WU-34. Recorded (never
   * undone) here - WU-34 reads it and honors it.
   */
  struct withheld_fk
  {
    std::string child;
    std::string parent;
    std::string name;
    std::string reason;
  };

  /*
   * One deferred plain index that did NOT build. Plain indexes are non-unique,
   * so they never hit the duplicate-key path - a failure here is a disk-full, a
   * btree_load_index error, or the like. It still makes the phase PARTIAL, so it
   * has to be a RECORD and not just a flag: a resumed run re-derives the phase
   * status from this summary, and before this existed a failed index left no
   * trace in the manifest, so the resume derived a clean OK and silently dropped
   * the non-zero exit an uninterrupted run would have returned.
   */
  struct failed_index
  {
    std::string name;
    std::string reason;
  };

  /*
   * The Rebuild phase outcome record - the manifest [rebuild] section source.
   * rebuilt lists the PK/UNIQUE re-added successfully; pending lists those that
   * failed (the exit-non-zero signal); indexes names the deferred plain indexes
   * built and failed_indexes those that did not; withheld lists the FK edges the
   * cascade withheld from WU-34.
   */
  struct rebuild_summary
  {
    std::vector<rebuilt_constraint> rebuilt;
    std::vector<pending_rebuild> pending;
    std::vector<std::string> indexes;
    std::vector<failed_index> failed_indexes;
    std::vector<withheld_fk> withheld;
  };

  /*
   * Rebuild-phase outcome. OK means every PK/UNIQUE re-added and every deferred
   * plain index built cleanly. PARTIAL means at least one PK/UNIQUE (or index)
   * failed: the failures are recorded in summary (pending + withheld) and the
   * caller still commits the partial result but exits non-zero. ERR_REBUILD is an
   * unexpected hard error (a rebuild source file could not be opened) already
   * reported by rebuild () - the caller aborts.
   */
  enum class rebuild_status
  {
    OK = 0,
    PARTIAL,			/* a PK/UNIQUE/index failed; recorded, run exits non-zero */
    ERR_REBUILD			/* a rebuild source file could not be opened */
  };

  /*
   * Re-add the stripped PK/UNIQUE constraints and build the deferred plain
   * indexes on the (loaded) target by re-executing the dump's own PK/UK ADD
   * statements (SPLIT: the <prefix>_schema_pk / <prefix>_schema_uk files; DEFAULT:
   * the PK/UK ALTER CLASS statements isolated from <prefix>_schema by matching the
   * stripped PK/UK names) then the <prefix>_indexes CREATE INDEXes, one statement
   * at a time on the already-open session with authorization disabled. Records the
   * outcome in summary (rebuilt / pending / indexes / withheld). Enforces the §6
   * rebuild-failure contract: a PK/UNIQUE that fails (duplicate key) is left
   * un-rebuilt, recorded in summary.pending, its parent-side FK edges recorded in
   * summary.withheld, and the phase continues - returning PARTIAL so the caller
   * commits the partial result and exits non-zero. Returns OK when everything
   * rebuilt cleanly, or ERR_REBUILD (already reported) on a hard file error. The
   * caller owns the transaction boundary.
   *
   * WU-50 resume: when present is non-null (this is the phase a resumed run
   * re-enters, so an interrupted prior rebuild may already have re-added part of
   * the set) a PK/UNIQUE the catalog already holds is recorded as rebuilt
   * without re-executing its ADD, and an index already built is recorded without
   * re-creating it - re-running either would fail on "already exists" and turn a
   * completed rebuild into a false PARTIAL.
   */
  rebuild_status rebuild (const import_set &iset, const dependency_graph &graph,
			  const std::vector<stripped_constraint> &stripped, rebuild_summary &summary,
			  const catalog_state *present = nullptr);

} // namespace cubimport

#endif /* _IMPORT_REBUILD_HPP_ */
