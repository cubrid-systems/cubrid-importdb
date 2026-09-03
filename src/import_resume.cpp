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
 * import_resume.cpp - Resume-safety helpers (WU-50, Gap-R)
 *
 * The catalog read and the data-phase reconcile a resumed run needs. Both run
 * on the already-open shared session (import_session.hpp) with authorization
 * disabled, exactly as the Graph builder's catalog snapshot and the Strip
 * phase's DDL do, and neither commits or aborts - the caller owns the
 * transaction boundary.
 */

#include "import_resume.hpp"

#include "db.h"
#include "dbtype.h"
#include "authenticate.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <string>
#include <vector>

namespace
{
  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  /* Read one column of the current tuple as a string; NULL -> "". Mirrors the
   * Graph builder's cell reader (the catalog columns here are all strings). */
  std::string
  cell_string (DB_QUERY_RESULT *result, int idx)
  {
    DB_VALUE value;
    std::string out;

    if (db_query_get_tuple_value (result, idx, &value) != NO_ERROR)
      {
	return out;
      }
    if (!DB_IS_NULL (&value))
      {
	const char *s = db_get_string (&value);
	if (s != NULL)
	  {
	    out = s;
	  }
      }
    db_value_clear (&value);
    return out;
  }

  /*
   * Run one SELECT and collect ncols string columns per row into rows. Returns
   * NO_ERROR on success, or the negative db_execute error code.
   */
  int
  collect_rows (const char *sql, int ncols, std::vector<std::vector<std::string>> &rows)
  {
    DB_QUERY_RESULT *result = NULL;
    DB_QUERY_ERROR query_error;

    int res = db_execute (sql, &result, &query_error);
    if (res < 0)
      {
	return res;
      }
    if (res > 0)
      {
	int pos = db_query_first_tuple (result);
	while (pos == DB_CURSOR_SUCCESS)
	  {
	    std::vector<std::string> row;
	    row.reserve (ncols);
	    for (int i = 0; i < ncols; i++)
	      {
		row.push_back (cell_string (result, i));
	      }
	    rows.push_back (std::move (row));
	    pos = db_query_next_tuple (result);
	  }
      }
    db_query_end (result);
    return NO_ERROR;
  }

  /*
   * True when class cls holds no rows, with authorization disabled as every
   * other catalog read in the pipeline is. One row answers the question, so the
   * query stops there rather than counting a table that may be large - and the
   * row is never READ: db_execute already reports how many rows a SELECT
   * produced, which is the whole answer. (Reading it was the first version's
   * bug: the tuple reader here handles string columns, and a `SELECT 1` column
   * is an integer.) Returns false on a query error - the caller reports that as
   * a failed check, never as an empty table.
   */
  bool
  class_is_empty (const std::string &cls, bool &empty)
  {
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    DB_QUERY_RESULT *result = NULL;
    DB_QUERY_ERROR query_error;
    const std::string sql = "SELECT 1 FROM [" + cls + "] LIMIT 1";
    const int rows = db_execute (sql.c_str (), &result, &query_error);
    if (rows >= 0)
      {
	db_query_end (result);
      }

    AU_RESTORE (au_save);

    if (rows < 0)
      {
	return false;
      }
    empty = (rows == 0);
    return true;
  }
} // namespace

namespace cubimport
{

  bool
  catalog_state::has_index (const std::string &cls, const std::string &name) const
  {
    return indexes.count (cls + "\t" + name) > 0;
  }

  bool
  catalog_state::has_class (const std::string &cls) const
  {
    return classes.count (cls) > 0;
  }

  bool
  verify_resume_target (const dependency_graph &graph, const std::vector<stripped_constraint> &stripped,
			bool constraints_must_be_absent, bool classes_must_be_empty, const catalog_state &present,
			std::string &why)
  {
    for (const graph_node &n : graph.nodes)
      {
	if (!present.has_class (n.name))
	  {
	    why = "class '" + n.name + "', which the manifest's dependency graph names, does not exist in this "
		  "database (it was dropped and recreated, or this is a different database of the same name)";
	    return false;
	  }
      }

    if (constraints_must_be_absent)
      {
	for (const stripped_constraint &c : stripped)
	  {
	    if (present.has_index (c.cls, c.name))
	      {
		why = "the manifest records constraint [" + c.name + "] on '" + c.cls
		      + "' as dropped, but this database still has it, so this database is not in the state the "
		      "interrupted run left behind";
		return false;
	      }
	  }
      }

    if (classes_must_be_empty)
      {
	for (const graph_node &n : graph.nodes)
	  {
	    bool empty = true;
	    if (!class_is_empty (n.name, empty))
	      {
		why = "class '" + n.name + "' could not be read to check that it is still empty";
		return false;
	      }
	    if (!empty)
	      {
		why = "the manifest reached only the definition phase, so every table should still be empty, but '"
		      + n.name + "' holds rows - this is not the empty target that run was defining, and resuming "
		      "would drop this database's constraints and append the dump's rows to its own";
		return false;
	      }
	  }
      }
    return true;
  }

  bool
  read_catalog_state (const std::string &database_name, catalog_state &st)
  {
    st = catalog_state ();

    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);
    std::vector<std::vector<std::string>> idx_rows, class_rows;
    /* The same class filter the Graph builder uses - the catalog view's own
     * predicate plus is_partition_pseudo () - so the two inventories are keyed
     * identically: unqualified class_name from the same catalog views, and no
     * partition pseudo-class in either. */
    int error = collect_rows ("SELECT class_name, index_name FROM db_index", 2, idx_rows);
    error = error ? error : collect_rows ("SELECT class_name FROM db_class "
					  "WHERE is_system_class='NO' AND class_type='CLASS'", 1, class_rows);
    AU_RESTORE (au_save);

    if (error != NO_ERROR)
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_CATALOG_QUERY_FAILED), database_name.c_str (), db_error_string (3));
	return false;
      }

    for (const std::vector<std::string> &r : idx_rows)
      {
	st.indexes.insert (r[0] + "\t" + r[1]);
      }
    for (const std::vector<std::string> &r : class_rows)
      {
	if (!is_partition_pseudo (r[0]))
	  {
	    st.classes.insert (r[0]);
	  }
      }
    return true;
  }

  bool
  truncate_classes (const dependency_graph &graph, int &truncated)
  {
    truncated = 0;

    /* As the Strip phase does, DBA-group members run this DDL with
     * authorization disabled. */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    bool ok = true;
    for (const graph_node &n : graph.nodes)
      {
	/* A partition's rows are emptied through its partitioned parent; a
	 * subclass keeps its own heap and is a node of its own. */
	DB_QUERY_RESULT *result = NULL;
	DB_QUERY_ERROR query_error;
	const std::string stmt = "TRUNCATE TABLE [" + n.name + "];";

	int error = db_execute (stmt.c_str (), &result, &query_error);
	if (error < 0)
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_TRUNCATE_FAILED), n.name.c_str (), db_error_string (3));
	    ok = false;
	    break;
	  }
	db_query_end (result);
	truncated++;
      }

    AU_RESTORE (au_save);
    return ok;
  }

} // namespace cubimport
