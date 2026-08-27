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
 * import_discovery.hpp - Stage 1 (Discovery) of the importdb pipeline
 *
 * Discovery rosters the files of an unloaddb dump directory into an ImportSet
 * and validates the set for completeness/consistency BEFORE any load begins
 * (10-design.md §3). It runs purely on the filesystem - no data load, no
 * server connection, no catalog access (those are later work units).
 */

#ifndef _IMPORT_DISCOVERY_HPP_
#define _IMPORT_DISCOVERY_HPP_

#include <string>
#include <vector>

namespace cubimport
{

  /*
   * How the schema DDL is laid out in the dump. DEFAULT is a single
   * <prefix>_schema file (also the O3 fallback for old dumps); SPLIT is the
   * unloaddb --split-schema-files layout: a bare <prefix>_schema_class plus
   * per-bucket constraint files, ordered by the <prefix>_schema_info manifest.
   */
  enum class schema_layout
  {
    DEFAULT,
    SPLIT
  };

  /*
   * How the object (row data) roster is laid out. SINGLE is one
   * <prefix>_objects file; PER_CLASS is the --datafile-per-class layout with a
   * <prefix>_<owner>.<class>_objects file per class.
   */
  enum class object_layout
  {
    SINGLE,
    PER_CLASS
  };

  /*
   * Discovery outcome. OK means the set rostered and validated cleanly; every
   * other value corresponds to a named diagnostic already emitted by discover.
   */
  enum class discover_status
  {
    OK = 0,
    ERR_DIR_OPEN,
    ERR_NO_DUMP,
    ERR_NO_SCHEMA,
    ERR_SCHEMA_INFO_MISSING,
    ERR_PREFIX_MISMATCH,
    ERR_NO_OBJECTS
  };

  /*
   * The ImportSet - the file roster plus the target-DB identity produced by
   * Discovery and consumed by later stages (10-design.md §2/§3). File members
   * hold basenames relative to dump_dir; an absent optional artifact is empty.
   */
  struct import_set
  {
    std::string database_name;			/* target-DB identity (1st CLI positional) */
    std::string dump_dir;			/* dump directory (2nd CLI positional) */
    std::string prefix;				/* derived dump prefix, e.g. "fx_ssb" */

    schema_layout schema_kind = schema_layout::DEFAULT;
    object_layout object_kind = object_layout::SINGLE;

    /* schema artifact(s) */
    std::string schema_file;			/* DEFAULT: <prefix>_schema */
    std::string schema_class_file;		/* SPLIT: bare <prefix>_schema_class */
    std::string schema_info_file;		/* SPLIT: <prefix>_schema_info manifest */
    std::vector<std::string> schema_apply_order;	/* SPLIT: manifest lines, in apply order */

    /* index and trigger artifacts (empty when absent) */
    std::string index_file;			/* <prefix>_indexes */
    std::string trigger_file;			/* <prefix>_trigger */

    /* object roster: one file for SINGLE, one per class for PER_CLASS */
    std::vector<std::string> object_files;
  };

  /*
   * Roster and validate the dump at dump_dir for database_name into result.
   * On failure emits the matching named diagnostic and returns a non-OK
   * status; on success result holds the rostered set.
   */
  discover_status discover (const char *database_name, const char *dump_dir, import_set &result);

  /*
   * Print a concise summary (layout, prefix, rostered files, counts) of a
   * successfully rostered set to stdout.
   */
  void print_import_set_summary (const import_set &result);

} // namespace cubimport

#endif /* _IMPORT_DISCOVERY_HPP_ */
