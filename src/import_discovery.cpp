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
 * import_discovery.cpp - Stage 1 (Discovery) of the importdb pipeline
 *
 * Rosters an unloaddb dump directory into an ImportSet and validates the set
 * before any load. Discovery is filesystem-only: it derives the dump prefix,
 * classifies the artifacts by layout (DEFAULT single-schema or SPLIT
 * --split-schema-files; SINGLE or PER_CLASS object roster), reads the
 * <prefix>_schema_info manifest as the source of truth for the split schema
 * files, and fails with a single named diagnostic when the set is incomplete
 * or inconsistent. All validation diagnostics are centralized here.
 */

#include "import_discovery.hpp"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <sys/stat.h>
#include <dirent.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace
{
  /* Every basename suffix that marks a recognizable unloaddb dump artifact.
   * Used to spot a foreign-prefix artifact (prefix mismatch). */
  const char *const DUMP_ARTIFACT_SUFFIXES[] =
  {
    "_schema_info", "_schema_class", "_schema_pk", "_schema_uk", "_schema_fk",
    "_schema", "_indexes", "_trigger", "_objects"
  };

  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  bool
  str_ends_with (const std::string &s, const char *suffix)
  {
    const size_t n = std::strlen (suffix);
    return s.size () >= n && s.compare (s.size () - n, n, suffix) == 0;
  }

  bool
  str_starts_with (const std::string &s, const std::string &prefix)
  {
    return s.size () >= prefix.size () && s.compare (0, prefix.size (), prefix) == 0;
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

  bool
  is_regular_file (const std::string &path)
  {
    struct stat st;
    return stat (path.c_str (), &st) == 0 && S_ISREG (st.st_mode);
  }

  bool
  looks_like_dump_artifact (const std::string &name)
  {
    for (const char *suffix : DUMP_ARTIFACT_SUFFIXES)
      {
	if (str_ends_with (name, suffix))
	  {
	    return true;
	  }
      }
    return false;
  }

  /* Derive the common dump prefix from the strongest available anchor. Schema
   * artifacts are preferred; a single <prefix>_objects file (no owner.class in
   * its stem) is the last resort. */
  bool
  derive_prefix (const std::vector<std::string> &entries, std::string &prefix)
  {
    static const char *const anchors[] =
    {
      "_schema_info", "_schema_class", "_schema", "_indexes", "_trigger"
    };
    for (const char *anchor : anchors)
      {
	for (const std::string &e : entries)
	  {
	    if (str_ends_with (e, anchor))
	      {
		prefix = e.substr (0, e.size () - std::strlen (anchor));
		return !prefix.empty ();
	      }
	  }
      }
    for (const std::string &e : entries)
      {
	if (str_ends_with (e, "_objects"))
	  {
	    std::string stem = e.substr (0, e.size () - std::strlen ("_objects"));
	    if (!stem.empty () && stem.find ('.') == std::string::npos)
	      {
		prefix = stem;
		return true;
	      }
	  }
      }
    return false;
  }

  /* Read a <prefix>_schema_info manifest into lines (trimmed, blanks skipped). */
  bool
  read_manifest (const std::string &path, std::vector<std::string> &lines)
  {
    std::ifstream in (path);
    if (!in.is_open ())
      {
	return false;
      }
    std::string line;
    while (std::getline (in, line))
      {
	const size_t first = line.find_first_not_of (" \t\r\n");
	if (first == std::string::npos)
	  {
	    continue;
	  }
	const size_t last = line.find_last_not_of (" \t\r\n");
	lines.push_back (line.substr (first, last - first + 1));
      }
    return true;
  }

  int
  count_constraint_files (const std::vector<std::string> &apply_order)
  {
    int count = 0;
    for (const std::string &f : apply_order)
      {
	if (str_ends_with (f, "_schema_pk") || str_ends_with (f, "_schema_uk") || str_ends_with (f, "_schema_fk"))
	  {
	    count++;
	  }
      }
    return count;
  }
} // namespace

namespace cubimport
{

  discover_status
  discover (const char *database_name, const char *dump_dir, import_set &result)
  {
    result = import_set ();
    result.database_name = database_name;
    result.dump_dir = dump_dir;

    /* 1. read the directory (regular files only). */
    DIR *dirp = opendir (dump_dir);
    if (dirp == NULL)
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_DIR_OPEN_FAILED), dump_dir);
	return discover_status::ERR_DIR_OPEN;
      }

    std::vector<std::string> entries;
    struct dirent *dp;
    while ((dp = readdir (dirp)) != NULL)
      {
	std::string name = dp->d_name;
	if (name == "." || name == "..")
	  {
	    continue;
	  }
	if (is_regular_file (path_join (dump_dir, name)))
	  {
	    entries.push_back (name);
	  }
      }
    closedir (dirp);

    /* deterministic classification / diagnostics regardless of readdir order */
    std::sort (entries.begin (), entries.end ());

    /* 2. derive the dump prefix. */
    std::string prefix;
    if (entries.empty () || !derive_prefix (entries, prefix))
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_NO_DUMP), dump_dir);
	return discover_status::ERR_NO_DUMP;
      }
    result.prefix = prefix;
    const std::string pfx = prefix + "_";

    /* 3. classify each file; a foreign-prefix artifact is a prefix mismatch. */
    for (const std::string &e : entries)
      {
	if (str_starts_with (e, pfx))
	  {
	    if (e == prefix + "_schema")
	      {
		result.schema_file = e;
	      }
	    else if (e == prefix + "_schema_class")
	      {
		result.schema_class_file = e;
	      }
	    else if (e == prefix + "_schema_info")
	      {
		result.schema_info_file = e;
	      }
	    else if (e == prefix + "_indexes")
	      {
		result.index_file = e;
	      }
	    else if (e == prefix + "_trigger")
	      {
		result.trigger_file = e;
	      }
	    else if (e == prefix + "_objects")
	      {
		result.object_files.push_back (e);
		result.object_kind = object_layout::SINGLE;
	      }
	    else if (str_ends_with (e, "_objects"))
	      {
		result.object_files.push_back (e);
		result.object_kind = object_layout::PER_CLASS;
	      }
	    /* pk/uk/fk files are rostered from _schema_info below; any other
	     * <prefix>_* file (e.g. _unloaddb.log, an importdb manifest) is
	     * tolerated and left un-rostered. */
	  }
	else if (looks_like_dump_artifact (e))
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_PREFIX_MISMATCH), e.c_str (), prefix.c_str ());
	    return discover_status::ERR_PREFIX_MISMATCH;
	  }
      }

    /* 4. resolve the schema layout and validate schema completeness. */
    if (!result.schema_info_file.empty ())
      {
	result.schema_kind = schema_layout::SPLIT;

	std::vector<std::string> manifest;
	if (!read_manifest (path_join (dump_dir, result.schema_info_file), manifest))
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_SCHEMA_INFO_MISSING), result.schema_info_file.c_str (),
				   result.schema_info_file.c_str ());
	    return discover_status::ERR_SCHEMA_INFO_MISSING;
	  }
	for (const std::string &named : manifest)
	  {
	    if (!is_regular_file (path_join (dump_dir, named)))
	      {
		PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_SCHEMA_INFO_MISSING), result.schema_info_file.c_str (),
				       named.c_str ());
		return discover_status::ERR_SCHEMA_INFO_MISSING;
	      }
	  }
	result.schema_apply_order = manifest;
      }
    else if (!result.schema_file.empty ())
      {
	result.schema_kind = schema_layout::DEFAULT;
      }
    else
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_NO_SCHEMA), dump_dir, prefix.c_str ());
	return discover_status::ERR_NO_SCHEMA;
      }

    /* 5. an object roster must be present. */
    if (result.object_files.empty ())
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_NO_OBJECTS), dump_dir, prefix.c_str ());
	return discover_status::ERR_NO_OBJECTS;
      }

    return discover_status::OK;
  }

  void
  print_import_set_summary (const import_set &result)
  {
    const char *layout = (result.schema_kind == schema_layout::SPLIT) ? "split" : "default";

    std::string schema_desc;
    if (result.schema_kind == schema_layout::SPLIT)
      {
	schema_desc = result.schema_class_file + " + " + std::to_string (count_constraint_files (result.schema_apply_order))
		      + " constraint file(s) (order from " + result.schema_info_file + ")";
      }
    else
      {
	schema_desc = result.schema_file + " (single)";
      }

    std::string object_desc;
    if (result.object_kind == object_layout::PER_CLASS)
      {
	object_desc = "per-class (" + std::to_string (result.object_files.size ()) + " file(s))";
      }
    else
      {
	object_desc = "single (" + result.object_files.front () + ")";
      }

    const std::string index_desc = result.index_file.empty () ? "(none)" : result.index_file;
    const std::string trigger_desc = result.trigger_file.empty () ? "(none)" : result.trigger_file;

    fprintf (stdout, msg (IMPORTDB_MSG_DISCOVERY_SUMMARY), layout, result.prefix.c_str (), result.dump_dir.c_str (),
	     schema_desc.c_str (), object_desc.c_str (), index_desc.c_str (), trigger_desc.c_str ());
  }

} // namespace cubimport
