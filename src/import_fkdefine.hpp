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
 * import_fkdefine.hpp - Stage 4 (FK define, step 5 of the constraint lifecycle)
 *
 * After the Rebuild phase (WU-32) restores PK/UNIQUE + indexes and the FK
 * re-validation phase (WU-33) checks every FK edge's child rows against its
 * parent, the FK define phase re-adds the FK constraints - the terminal step of
 * the §6 constraint lifecycle (10-design.md §6 / Q10). It defines the FK ONLY on
 * edges that validated clean: on a populated (validated-clean) table an
 * ADD ... FOREIGN KEY bulk-builds the FK b-tree bottom-up server-side. This
 * closes the round-trip - a clean dump ends with catalog == snapshot (every
 * PK/UK/FK/index present) - and, because the FK is defined only on data already
 * proven clean, FM5 (a defined FK trusting orphan rows) cannot occur: CUBRID's
 * ADD FOREIGN KEY does not itself re-check the rows.
 *
 * An edge is WITHHELD (its FK is NOT defined) when the Rebuild phase withheld it
 * (rebuild_summary.withheld - its parent PK failed to rebuild, so there is no
 * parent PK index to reference) or the FK re-validation phase found orphans on it
 * (validate_summary - the edge is violated). Under fail-fast an edge that
 * re-validation never reached (it stopped at an earlier violation) is not proven
 * clean either and is withheld too. A withheld edge's FK is skipped and its exact
 * ADD CONSTRAINT ... FOREIGN KEY re-add DDL is recorded (manifest [fkdefine]
 * section + appended to the exceptions artifact) so an operator can add it after
 * repairing the data.
 *
 * The FK define re-executes the dump's OWN FK ADD statements verbatim (mirroring
 * the Rebuild phase's PK/UK re-execution) rather than synthesizing DDL: for a
 * SPLIT dump it runs the isolated <prefix>_schema_fk file(s) (named in
 * iset.schema_apply_order); for a DEFAULT dump it isolates, from the single
 * <prefix>_schema text, the ALTER CLASS ... ADD CONSTRAINT [<name>] FOREIGN KEY
 * statements whose constraint name matches the graph's FK edge names (the FK form
 * ADD CONSTRAINT ... FOREIGN KEY is distinct from the PK/UK form ADD ATTRIBUTE
 * CONSTRAINT ... PRIMARY KEY|UNIQUE). Statements run one at a time on the already-
 * open session with the same public single-statement primitive the other phases
 * use (db_execute), authorization disabled for the DBA-group path. loaddb is not
 * touched.
 *
 * FK-define contract (10-design.md §6 / Q10): every clean edge's FK is defined
 * and every withheld edge is recorded. All-clean -> OK (catalog == snapshot). Any
 * withheld edge -> PARTIAL: the clean edges' FK is still defined and committed,
 * the withheld edges are recorded, and the run exits non-zero (already non-zero
 * from WU-32/33). A hard error defining a CLEAN edge's FK (unexpected - the data
 * validated clean), a source file that cannot be opened, or an exceptions-artifact
 * write failure is ERR_FKDEFINE - the caller aborts. The caller owns the
 * transaction (import_session.hpp): define_fks () neither commits nor aborts.
 */

#ifndef _IMPORT_FKDEFINE_HPP_
#define _IMPORT_FKDEFINE_HPP_

#include "import_discovery.hpp"
#include "import_graph.hpp"
#include "import_rebuild.hpp"
#include "import_validate.hpp"

#include <string>
#include <vector>

namespace cubimport
{

  /* import_resume.hpp - the WU-50 resume guard. Only ever taken by pointer
   * here, and declaring it keeps the include graph acyclic (import_resume.hpp
   * needs this header for stripped_constraint). */
  struct catalog_state;

  /* One FK defined successfully on a validated-clean edge (child -> parent, the
   * FK constraint name). The defined set are the FKs the round-trip restored. */
  struct defined_fk
  {
    std::string child;
    std::string parent;
    std::string name;
  };

  /*
   * One FK edge withheld from the define: the edge (child -> parent, fk name), the
   * reason it was withheld (parent PK un-rebuilt, validation orphan, or - under
   * fail-fast - never re-validated), and the exact ADD CONSTRAINT ... FOREIGN KEY
   * statement an operator re-runs to add the FK after repairing the data. Recorded
   * in the manifest [fkdefine] section and appended to the exceptions artifact.
   */
  struct withheld_define
  {
    std::string child;
    std::string parent;
    std::string name;
    std::string reason;
    std::string readd_ddl;
  };

  /*
   * The FK-define outcome record - the manifest [fkdefine] section source. defined
   * lists the FKs re-added on validated-clean edges; withheld lists the edges left
   * undefined with their re-add DDL; exceptions_file is the exceptions artifact
   * basename when any withheld FK's re-add DDL was recorded there (empty when no
   * edge was withheld).
   */
  struct fkdefine_summary
  {
    std::vector<defined_fk> defined;
    std::vector<withheld_define> withheld;
    std::string exceptions_file;
  };

  /*
   * FK-define outcome. OK means every FK edge was defined (all edges validated
   * clean - catalog == snapshot, the full round-trip). PARTIAL means at least one
   * edge was withheld: the clean edges' FK is defined and the withheld edges are
   * recorded (manifest + exceptions artifact), the caller commits the partial
   * result but exits non-zero. ERR_FKDEFINE is a hard error already reported by
   * define_fks () (a source file could not be opened, defining a CLEAN edge's FK
   * failed unexpectedly, or the exceptions artifact could not be written) - the
   * caller aborts.
   */
  enum class fkdefine_status
  {
    OK = 0,
    PARTIAL,			/* one or more edges withheld; recorded, run exits non-zero */
    ERR_FKDEFINE		/* source-file open / clean-edge define / exceptions-write error */
  };

  /*
   * Define the FK on every validated-clean edge of the graph by re-executing the
   * dump's own FK ADD statements (SPLIT: the isolated <prefix>_schema_fk file(s);
   * DEFAULT: the ADD CONSTRAINT ... FOREIGN KEY statements isolated from
   * <prefix>_schema by matching the graph's FK edge names) on the already-open
   * session with authorization disabled. An edge the Rebuild phase withheld
   * (rebuild.withheld), the FK re-validation phase found violated, or (under
   * fail-fast) never re-validated is skipped and recorded in summary.withheld with
   * its exact re-add DDL; the record is appended to the exceptions artifact in
   * iset.dump_dir. Records the outcome in summary (defined / withheld). Returns OK
   * when every edge was defined, PARTIAL when at least one edge was withheld (the
   * caller commits and exits non-zero), or ERR_FKDEFINE (already reported) on a
   * hard error. The caller owns the transaction boundary.
   *
   * WU-50 resume: when present is non-null (this is the phase a resumed run
   * re-enters, so an interrupted prior define may already have defined part of
   * the set) a clean edge whose FK the catalog already holds is recorded as
   * defined without re-executing its ADD - re-executing it would fail on
   * "already exists", which this phase treats as a hard error.
   */
  fkdefine_status define_fks (const import_set &iset, const dependency_graph &graph, const rebuild_summary &rebuild,
			      bool continue_on_error, validate_summary &validate, fkdefine_summary &summary,
			      const catalog_state *present = nullptr);

} // namespace cubimport

#endif /* _IMPORT_FKDEFINE_HPP_ */
