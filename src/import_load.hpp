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
 * import_load.hpp - Stage 4 (Load phase, step 2 of the constraint lifecycle)
 *
 * After the Strip phase (WU-30) leaves bare heaps, the Load phase pours each
 * table's object data into those heaps by REUSING the loaddb data-load path -
 * loaddb source is not touched. It drives the load through loaddb's PUBLIC
 * client stubs only (network_interface_cl.h + load_common.hpp):
 *   loaddb_init (load_args)     - set up the server-side load session + the
 *                                 intra-table worker pool (reused as-is),
 *   cubload::split ()           - parse one object file into batches, invoking
 *                                 the class_handler / batch_handler,
 *   loaddb_install_class () / loaddb_load_batch () - the handlers' bodies,
 *   loaddb_fetch_status ()      - drain stats / errors,
 *   loaddb_destroy () / loaddb_interrupt () - tear down / abort.
 * This mirrors load_db.c's ldr_server_load () + load_object_file (), dropping
 * the loaddb-only concerns (logddl, -S/SA paths, estimated size, storage-order
 * compare, statistics update - statistics are rebuilt in a later WU).
 *
 * Loading is serial across tables (inter-table parallelism is M4/WU-40; only
 * the intra-table worker pool is reused here). Each object file is loaded in
 * its own loaddb session (loaddb_init .. loaddb_destroy), which matches loaddb's
 * one-file-per-session model and keeps split ()'s per-file class-id numbering
 * independent: a SINGLE-layout dump has one object file carrying every class
 * (one session loads them all), a PER_CLASS-layout dump has one file per class
 * (one session each). Object-valued classes excluded by --skip-object-classes
 * (graph.skipped_classes) are not loaded. No constraint is rebuilt and no FK is
 * validated/defined here (later WUs); after this phase the target holds data in
 * bare heaps.
 */

#ifndef _IMPORT_LOAD_HPP_
#define _IMPORT_LOAD_HPP_

#include "import_discovery.hpp"
#include "import_graph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cubimport
{

  /* Rows loaded from one object file (its own loaddb session): the file's
   * basename and the committed/failed row counts reported by loaddb's stats.
   * For PER_CLASS this is per-class; for SINGLE the one entry is the whole
   * dump's total. */
  struct load_file_result
  {
    std::string object_file;
    int64_t rows = 0;
    int64_t failed = 0;
  };

  /*
   * The Load phase outcome record - the manifest [load] section source. files
   * lists the per-file results in load order; total_rows/total_failed are the
   * sums; loaded_files counts the files actually loaded (skipped classes
   * excluded).
   */
  struct load_summary
  {
    std::vector<load_file_result> files;
    int64_t total_rows = 0;
    int64_t total_failed = 0;
    int loaded_files = 0;
  };

  /*
   * Load-phase outcome. OK means every object file loaded and the target now
   * holds data in bare heaps; ERR_LOAD corresponds to a named diagnostic
   * already emitted by load_data ().
   */
  enum class load_data_status
  {
    OK = 0,
    ERR_LOAD			/* a loaddb child failed to load an object file */
  };

  /*
   * Load the ImportSet's object data into the (stripped, bare-heap) target.
   *
   * The CUBRID CS client is one-connection-per-process (a single global
   * connection/request socket), so concurrency cannot come from threads sharing
   * the caller's session. The data phase therefore spawns independent
   * `cub_admin loaddb -C -d <object-file>` child PROCESSES -- each with its own
   * connection and server-side worker pool -- up to `degree` at a time, bounded
   * also by the object-file fan-out. Serial is degree 1: the same path, one child
   * at a time. This is the model WU-05 validated (Q5: no cross-class BU_LOCK
   * hazard for independent tables).
   *
   * There is deliberately no in-process variant. Driving loaddb's client stubs
   * directly means calling entry points that take cubload C++ types by
   * reference, which binds this utility to the engine's libstdc++ ABI; the
   * published CUBRID binaries use the pre-C++11 std::string ABI, which a modern
   * compiler does not produce. The subprocess path keeps this utility on the
   * extern "C" db_* API, where that coupling does not exist.
   *
   * Because the children are separate transactions the caller MUST have
   * committed the strip first (session_commit), so they load into durable bare
   * heaps. Object-file order is irrelevant (bare heaps, value-only data), so all
   * files are eligible to run concurrently; a PER_CLASS dump (one file per class)
   * is what actually parallelizes, a SINGLE dump runs one child. user/password
   * are forwarded to each child's -u/-p. Skipped classes are honoured: a
   * PER_CLASS file for a skipped class is not spawned, a SINGLE file gets
   * --ignore-class-file. Row counts are read back from the catalog after the
   * children commit.
   *
   * On success appends each file's row counts to summary and returns OK. On any
   * child failure emits IMPORTDB_MSG_LOAD_FAILED naming the object file (with the
   * child's log tail) and returns ERR_LOAD; the caller aborts and closes the
   * session.
   */
  load_data_status load_data (const import_set &iset, const dependency_graph &graph, load_summary &summary,
			      int degree, const char *user, const char *password);

} // namespace cubimport

#endif /* _IMPORT_LOAD_HPP_ */
