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
 * import_validate.cpp - FK orphan enumeration (the FK define phase's failure path)
 *
 * Lists the orphan child rows of ONE FK edge by a set-based anti-join. Per edge
 * child -> parent it runs
 *   SELECT CAST(c.<pk/fk col> AS VARCHAR) ... FROM [child] AS c
 *   WHERE <every fk col> IS NOT NULL
 *     AND NOT EXISTS (SELECT 1 FROM [parent] AS p WHERE p.<pk col> = c.<fk col> [AND ...])
 * over the same public single-statement client primitive the Graph builder reads
 * the catalog with (db_execute), authorization disabled for the DBA-group path.
 * The result rows are exactly the orphan child rows. Child FK columns are paired
 * with parent PK columns POSITIONALLY (the mapping the Graph builder captured at
 * snapshot: fk_edge.child_columns <-> parent_pk_columns), which is why FK and
 * parent-PK column names need not match; a composite FK pairs several columns.
 * Identity/value columns are CAST to VARCHAR so every type reads back uniformly.
 * loaddb is not touched.
 *
 * THIS USED TO BE A PHASE OF ITS OWN, run over every edge before any FK was
 * defined, on the belief that CUBRID's ADD FOREIGN KEY does not check existing
 * rows. That belief was wrong. `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY`
 * builds the FK's b-tree over the rows already there (sm_add_constraint ->
 * btree_load_index) and btree_load_check_fk probes every key against the parent's
 * PK index as it goes, so an orphan makes the statement fail with ER_FK_INVALID
 * and no FK is created. Measured on nightly 11.5.0.2494 and on develop
 * d0b290459, for the ADD CONSTRAINT / ADD FOREIGN KEY / ALTER CLASS spellings and
 * for composite and cyclic edges: rejected in every case, leaving no catalog
 * residue and without poisoning the surrounding transaction.
 *
 * So a separate pass over clean data was pure cost -- about 0.56 s per edge
 * against about 0.2 s for the ADD itself once the child's pages are warm, which
 * they are right after the load. What the engine does NOT do is enumerate: it
 * reports the FIRST offending value and stops. That is what this file is for now.
 * The FK define phase attempts the ADD, and only when the engine rejects one does
 * it come here for the full list of offenders and the exceptions artifact. Clean
 * dumps never pay for it.
 */
#include "import_validate.hpp"

#include "db.h"
#include "dbtype.h"
#include "authenticate.h"
#include "environment_variable.h"	/* envvar_bindir_file - locate the csql binary (WU-41) */

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
  /* Basename of the importdb-written exceptions artifact inside the importdb
   * directory. Named like the manifest (importdb.<role>) so a later Discovery
   * re-scan (resume) tolerates it and it never collides with a dump artifact. */
  const char *const EXCEPTIONS_BASENAME = "importdb.exceptions";

  const int EXCEPTIONS_FORMAT_VERSION = 1;

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

  /* Comma-separated join of a string list (column-name lists in diagnostics). */
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

  /* Read one column of the current tuple as a string (the anti-join CASTs every
   * selected column to VARCHAR, so db_get_string yields the value directly; a
   * NULL - not expected for PK/filtered-FK columns - reads as "NULL"). */
  std::string
  cell_string (DB_QUERY_RESULT *result, int idx)
  {
    DB_VALUE value;
    std::string out = "NULL";

    if (db_query_get_tuple_value (result, idx, &value) != NO_ERROR)
      {
	return "";
      }
    if (!DB_IS_NULL (&value))
      {
	const char *s = db_get_string (&value);
	out = (s != NULL) ? std::string (s) : std::string ("");
      }
    db_value_clear (&value);
    return out;
  }

  /*
   * Run one SELECT and collect ncols string columns per row into rows. Returns
   * NO_ERROR on success (rows appended), or the negative db_execute error code.
   * Mirrors the Graph builder's catalog-read primitive.
   */
  int
  run_query (const std::string &sql, int ncols, std::vector<std::vector<std::string>> &rows)
  {
    DB_QUERY_RESULT *result = NULL;
    DB_QUERY_ERROR query_error;

    int res = db_execute (sql.c_str (), &result, &query_error);
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
   * The shared "FROM [child] AS c WHERE <every fk col> IS NOT NULL AND NOT EXISTS
   * (parent match)" tail of the anti-join for one FK edge (positional FK col <->
   * parent PK col equality; bracket-quoted; aliased so equal column names on the
   * two sides stay unambiguous). Both the enumeration SELECT (build_antijoin) and
   * the enumerating SELECT uses THIS same tail, so the
   * two paths test byte-identical predicates and can never disagree on clean vs
   * violated.
   */
  std::string
  antijoin_from_where (const cubimport::fk_edge &e)
  {
    std::string sql = " FROM [" + e.child + "] AS c WHERE ";
    for (size_t i = 0; i < e.child_columns.size (); i++)
      {
	sql += (i == 0 ? "" : " AND ");
	sql += "c.[" + e.child_columns[i] + "] IS NOT NULL";
      }

    sql += " AND NOT EXISTS (SELECT 1 FROM [" + e.parent + "] AS p WHERE ";
    for (size_t i = 0; i < e.child_columns.size (); i++)
      {
	sql += (i == 0 ? "" : " AND ");
	sql += "p.[" + e.parent_pk_columns[i] + "] = c.[" + e.child_columns[i] + "]";
      }
    sql += ")";
    return sql;
  }

  /*
   * Build the enumeration anti-join SELECT for one FK edge: the select list is the
   * child's PK columns (row identity) then the FK columns (offending value), each
   * CAST to VARCHAR so they read back as strings, over the shared FROM+WHERE. The
   * result rows are exactly the orphan child rows.
   */
  std::string
  build_antijoin (const cubimport::fk_edge &e)
  {
    std::string sql = "SELECT ";
    bool first = true;
    for (const std::string &col : e.child_pk_columns)
      {
	sql += (first ? "" : ", ");
	sql += "CAST(c.[" + col + "] AS VARCHAR)";
	first = false;
      }
    for (const std::string &col : e.child_columns)
      {
	sql += (first ? "" : ", ");
	sql += "CAST(c.[" + col + "] AS VARCHAR)";
	first = false;
      }
    sql += antijoin_from_where (e);
    return sql;
  }

  std::string
  join_pairs (const std::vector<std::string> &cols, const std::vector<std::string> &row, size_t offset)
  {
    std::string out;
    for (size_t i = 0; i < cols.size (); i++)
      {
	if (i != 0)
	  {
	    out += ", ";
	  }
	out += cols[i] + "=" + ((offset + i < row.size ()) ? row[offset + i] : std::string ());
      }
    return out;
  }

  /* Locate an FK edge by (child, parent, name) for its column lists. */
  const cubimport::fk_edge *
  find_edge (const cubimport::dependency_graph &graph, const cubimport::fk_edge_result &r)
  {
    for (const cubimport::fk_edge &e : graph.fk_edges)
      {
	if (e.child == r.child && e.parent == r.parent && e.name == r.name)
	  {
	    return &e;
	  }
      }
    return NULL;
  }

  /*
   * Render + write the exceptions artifact: a header (policy + tallies) then, per
   * violated edge, its column lists and one "orphan:" line per offending child
   * row. Human-readable and regular so an operator can repair from it. Returns
   * true on success; on failure leaves errno set for the caller's diagnostic.
   */
  bool
  write_exceptions (const std::string &path, const cubimport::import_set &iset,
		    const cubimport::dependency_graph &graph, const cubimport::validate_summary &summary,
		    bool continue_on_error)
  {
    std::ostringstream os;
    os << "# CUBRID importdb FK re-validation exceptions (format v" << EXCEPTIONS_FORMAT_VERSION << ")\n";
    os << "# Written and owned by importdb; do not edit by hand.\n";
    os << "# Each 'orphan' line is a child row whose foreign-key value has no matching parent primary key.\n";
    os << "# An operator repairs the data from this file; the FK on these edges is left undefined.\n";
    os << "generator: importdb\n";
    os << "created: " << utc_timestamp () << "\n";
    os << "database: " << iset.database_name << "\n";
    os << "policy: " << (continue_on_error ? "continue" : "fail-fast") << "\n";
    os << "violated_edges: " << summary.violated_edges << "\n";
    os << "total_orphans: " << summary.total_orphans << "\n";
    os << "\n";

    for (const cubimport::fk_edge_result &r : summary.edges)
      {
	if (r.skipped || r.orphans == 0)
	  {
	    continue;
	  }
	const cubimport::fk_edge *e = find_edge (graph, r);
	os << "[edge] " << r.child << " -> " << r.parent << " (" << r.name << ")\n";
	os << "child_key_columns: " << ((e != NULL && !e->child_pk_columns.empty ()) ? join_list (e->child_pk_columns)
					: std::string ("(none)")) << "\n";
	os << "fk_columns: " << (e != NULL ? join_list (e->child_columns) : std::string ()) << "\n";
	os << "orphans: " << r.orphans << "\n";
	for (const cubimport::fk_orphan &o : summary.orphans)
	  {
	    if (o.child == r.child && o.parent == r.parent && o.fk_name == r.name)
	      {
		os << "orphan: child[" << o.child_key << "] fk[" << o.fk_values << "]\n";
	      }
	  }
	os << "\n";
      }

    const std::string content = os.str ();
    FILE *fp = fopen (path.c_str (), "w");
    if (fp == NULL)
      {
	return false;
      }
    if (fwrite (content.data (), 1, content.size (), fp) != content.size ())
      {
	int saved = errno;
	fclose (fp);
	errno = saved;
	return false;
      }
    if (fclose (fp) != 0)
      {
	return false;
      }
    return true;
  }

} // namespace

namespace cubimport
{

  int
  enumerate_fk_orphans (const fk_edge &edge, std::vector<fk_orphan> &out)
  {
    /* The column mapping is captured at snapshot; a real FK edge always has
     * matching-arity child + parent-PK columns. A malformed mapping means the
     * anti-join cannot be built correctly -- report and fail, do not guess. */
    if (edge.child_columns.empty () || edge.parent_pk_columns.empty ()
	|| edge.child_columns.size () != edge.parent_pk_columns.size ())
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_VALIDATE_QUERY_FAILED), edge.child.c_str (), edge.parent.c_str (),
			       edge.name.c_str (), "missing or malformed FK-to-PK column mapping");
	return ER_FAILED;
      }

    /* As the other server-side phases do, DBA-group members run the anti-join
     * with authorization disabled. AU_SAVE_AND_DISABLE saves the prior value, so
     * nesting inside the FK define phase's window is safe. */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    const int ncols = (int) (edge.child_pk_columns.size () + edge.child_columns.size ());
    std::vector<std::vector<std::string>> rows;
    int error = run_query (build_antijoin (edge), ncols, rows);

    AU_RESTORE (au_save);

    if (error != NO_ERROR)
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_VALIDATE_QUERY_FAILED), edge.child.c_str (), edge.parent.c_str (),
			       edge.name.c_str (), db_error_string (3));
	return error;
      }

    const size_t pkcnt = edge.child_pk_columns.size ();
    for (const std::vector<std::string> &row : rows)
      {
	fk_orphan o;
	o.child = edge.child;
	o.parent = edge.parent;
	o.fk_name = edge.name;
	o.child_key = pkcnt ? join_pairs (edge.child_pk_columns, row, 0) : std::string ("(no pk)");
	o.fk_values = join_pairs (edge.child_columns, row, pkcnt);
	out.push_back (o);
      }
    return NO_ERROR;
  }

  bool
  write_fk_exceptions (const std::string &path, const import_set &iset, const dependency_graph &graph,
		       const validate_summary &summary, bool continue_on_error)
  {
    return write_exceptions (path, iset, graph, summary, continue_on_error);
  }

} // namespace cubimport
