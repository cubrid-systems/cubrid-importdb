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
 * import_resume.hpp - Resume-safety helpers (WU-50, Gap-R)
 *
 * A resumed run re-enters the pipeline at the phase AFTER the manifest's
 * `reached` marker. Because the marker is advanced only after the phase's work
 * has been committed (import_db.cpp writes the manifest immediately after a
 * session_commit ()), the phase that follows `reached` is the only one that can
 * have been interrupted mid-flight: every later phase in the resumed process
 * starts from a state that same process established. So exactly ONE phase per
 * resumed run needs a skip-what-is-already-done guard, and these are the two
 * things such a guard needs.
 *
 * catalog_state is a read of the target's CURRENT index inventory. It is the
 * only authority on how far the interrupted phase actually got: the manifest
 * records what the prior run intended and completed, the catalog records what
 * survived. The Strip phase drops only a constraint still present, the Rebuild
 * phase re-adds only one still absent, the FK define phase defines only an FK
 * still absent - all keyed off this one read.
 *
 * truncate_classes () is the Load phase's reconcile step. A killed data phase
 * leaves an unknown number of committed rows (loaddb commits periodically, and
 * the --degree > 1 children commit independently), and no per-row resume point
 * exists. Since the tables are bare heaps at that point, emptying them is cheap
 * and makes the reload exact rather than approximately-right.
 */

#ifndef _IMPORT_RESUME_HPP_
#define _IMPORT_RESUME_HPP_

#include "import_graph.hpp"
#include "import_strip.hpp"	/* stripped_constraint */

#include <set>
#include <string>
#include <vector>

namespace cubimport
{

  /*
   * The target's current index inventory - every (class, index) pair the
   * catalog holds, PK / UNIQUE / FK / plain index alike, read in one query.
   * A constraint's presence here is what "already done" means for the phase
   * a resumed run re-enters.
   */
  struct catalog_state
  {
    std::set<std::string> indexes;	/* "<class>\t<index name>" */
    std::set<std::string> classes;	/* user class names */

    bool has_index (const std::string &cls, const std::string &name) const;
    bool has_class (const std::string &cls) const;
  };

  /*
   * Read the target's index inventory into st on the already-open session
   * (authorization disabled, as the Graph builder's catalog reads are).
   * Returns true on success; on a catalog-read failure emits the named
   * diagnostic (IMPORTDB_MSG_CATALOG_QUERY_FAILED) and returns false.
   */
  bool read_catalog_state (const std::string &database_name, catalog_state &st);

  /*
   * Decide whether the connected database is really the one the manifest
   * describes, before a resumed run touches it.
   *
   * The [set] identity is a database NAME, and a name is not an identity: two
   * hosts, or one host at two times, can hold different databases under it. That
   * matters more on the resume path than anywhere else, because a resumed run
   * SKIPS the definition phase - and `CREATE CLASS` failing on an existing class
   * is the check that would otherwise refuse a target that is not empty. What is
   * left is a TRUNCATE driven by a class list read out of a file.
   *
   * So the target has to prove it matches the record: every class the manifest's
   * graph names must exist, and then whichever of the two state tests the
   * manifest's own marker makes true:
   *
   *   constraints_must_be_absent - at STRIPPED, where the manifest asserts every
   *     recorded constraint was dropped and the destructive TRUNCATE is about to
   *     run. It must NOT be asked at LOADED: there an interrupted rebuild
   *     legitimately leaves part of the set back, which is the case the rebuild
   *     guard exists for.
   *   classes_must_be_empty - at DEFINED, where the strip phase is about to DROP
   *     every constraint the graph names. Emptiness is importdb's own invariant
   *     at that marker (the strip phase's contract is "on the still-empty defined
   *     tables", and the load phase follows the STRIPPED marker), so a legitimate
   *     resume always passes and a populated look-alike is caught before its
   *     constraints are dropped and the dump's rows are appended to its own.
   *
   * A populated database that merely shares the name fails one of the two; one
   * that was dropped and recreated between the runs fails the class-existence
   * test. Returns true when the target matches; on false, why explains which
   * check failed, in operator-facing terms.
   */
  bool verify_resume_target (const dependency_graph &graph, const std::vector<stripped_constraint> &stripped,
			     bool constraints_must_be_absent, bool classes_must_be_empty, const catalog_state &present,
			     std::string &why);

  /*
   * Empty every data class of the graph before a resumed data phase re-runs it,
   * so the reload starts from the same bare heaps a fresh run would find. The
   * classes are bare (their constraints are stripped) at this point, so TRUNCATE
   * has no constraint to save and restore and no FK referrer to refuse for; it
   * also resets each auto_increment serial to its start value, which is the
   * state the definition phase left it in (loaddb inserts explicit values and
   * never advances a serial). Runs on the already-open session and leaves the
   * transaction open for the caller. On success sets truncated to the number of
   * classes emptied and returns true; on a failure emits the named diagnostic
   * (IMPORTDB_MSG_TRUNCATE_FAILED) naming the class and returns false.
   */
  bool truncate_classes (const dependency_graph &graph, int &truncated);

} // namespace cubimport

#endif /* _IMPORT_RESUME_HPP_ */
