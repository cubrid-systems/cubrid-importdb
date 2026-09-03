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
 * import_graph.cpp - Stage 2 (Graph builder) of the importdb pipeline
 *
 * Reads the connected target's catalog (the snapshot step of the §4 lifecycle,
 * on the same open session as define) and builds the DependencyGraph. This
 * productizes the validated WU-03 prototype (m0/proto/wu03_graph.py): the same
 * catalog queries (11-design-detail.md §3), the same DFS cycle detection, and
 * the same Kahn level-set layering, ported to C++ over the public db_execute /
 * DB_QUERY_RESULT client API. Catalog reads run with authorization disabled so
 * underlying system classes (_db_serial, which the db_serial VIEW hides
 * AUTO_INCREMENT serials from) are readable by the DBA-group import user.
 */

#include "import_graph.hpp"
#include "import_progress.hpp"

#include "db.h"
#include "dbtype.h"
#include "authenticate.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  /* Read one column of the current tuple as a string: NULL -> "", a string
   * domain via db_get_string, an integer via db_get_int; anything else -> "". */
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
	else if (db_value_type (&value) == DB_TYPE_INTEGER)
	  {
	    out = std::to_string (db_get_int (&value));
	  }
      }
    db_value_clear (&value);
    return out;
  }

  /*
   * Run one SELECT and collect ncols string columns per row into rows. Returns
   * NO_ERROR on success (rows appended), or the negative db_execute error code.
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

  /* Comma-separated join of a string list (for diagnostics/summary). */
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

  /* "attr -> domain" pairs of a node's object columns, comma-joined. */
  std::string
  join_object_columns (const std::vector<std::pair<std::string, std::string>> &cols)
  {
    std::string out;
    for (size_t i = 0; i < cols.size (); i++)
      {
	if (i != 0)
	  {
	    out += ", ";
	  }
	out += cols[i].first + " -> " + cols[i].second;
      }
    return out;
  }

  /*
   * FK cycles as node lists (DFS over the FK edge set). Ported from the WU-03
   * prototype find_cycles: a back-edge to a GREY node records the stack slice
   * from that node plus the node itself (e.g. [emp, dept, emp]).
   */
  std::vector<std::vector<std::string>>
				     find_cycles (const std::vector<cubimport::fk_edge> &fk)
  {
    std::map<std::string, std::vector<std::string>> adj;
    for (const cubimport::fk_edge &e : fk)
      {
	adj[e.child].push_back (e.parent);
	/* ensure parents appear as keys so every node is visited */
	if (adj.find (e.parent) == adj.end ())
	  {
	    adj[e.parent];
	  }
      }

    enum
    { WHITE, GREY, BLACK };
    std::map<std::string, int> color;
    std::vector<std::string> stack;
    std::vector<std::vector<std::string>> cycles;

    std::function<void (const std::string &)> dfs = [&] (const std::string &u)
    {
      color[u] = GREY;
      stack.push_back (u);
      for (const std::string &v : adj[u])
	{
	  if (color[v] == GREY)
	    {
	      std::vector<std::string>::iterator it = std::find (stack.begin (), stack.end (), v);
	      std::vector<std::string> cycle (it, stack.end ());
	      cycle.push_back (v);
	      cycles.push_back (cycle);
	    }
	  else if (color[v] == WHITE)
	    {
	      dfs (v);
	    }
	}
      color[u] = BLACK;
      stack.pop_back ();
    };

    for (std::map<std::string, std::vector<std::string>>::iterator it = adj.begin (); it != adj.end (); ++it)
      {
	if (color[it->first] == WHITE)
	  {
	    dfs (it->first);
	  }
      }
    return cycles;
  }

  /*
   * Parallel-eligible level sets (Kahn layering). Ported from the WU-03
   * prototype level_sets: inheritance is a hard precedence (super before
   * child), FK a soft one (parent before child) that is dropped when both ends
   * are in a cycle (FK is off during load anyway, Q2).
   */
  std::vector<std::vector<std::string>>
				     level_sets (const std::vector<std::string> &nodes, const std::vector<cubimport::fk_edge> &fk,
					 const std::vector<cubimport::inherit_edge> &inh,
					 const std::vector<std::vector<std::string>> &cycles)
  {
    std::set<std::string> node_set (nodes.begin (), nodes.end ());
    std::set<std::string> cyc_nodes;
    for (const std::vector<std::string> &cy : cycles)
      {
	cyc_nodes.insert (cy.begin (), cy.end ());
      }

    /* precedence edges (pred -> succ): dedup via a set of pairs */
    std::set<std::pair<std::string, std::string>> edges;
    for (const cubimport::inherit_edge &e : inh)
      {
	edges.insert (std::make_pair (e.super, e.child));
      }
    for (const cubimport::fk_edge &e : fk)
      {
	if (cyc_nodes.count (e.parent) && cyc_nodes.count (e.child))
	  {
	    continue;		/* broken on cycle */
	  }
	edges.insert (std::make_pair (e.parent, e.child));
      }

    std::map<std::string, int> indeg;
    for (const std::string &n : nodes)
      {
	indeg[n] = 0;
      }
    std::map<std::string, std::vector<std::string>> succ;
    for (const std::pair<std::string, std::string> &e : edges)
      {
	if (node_set.count (e.first) && node_set.count (e.second))
	  {
	    succ[e.first].push_back (e.second);
	    indeg[e.second] += 1;
	  }
      }

    std::vector<std::string> frontier;
    for (const std::string &n : nodes)
      {
	if (indeg[n] == 0)
	  {
	    frontier.push_back (n);
	  }
      }
    std::sort (frontier.begin (), frontier.end ());

    std::vector<std::vector<std::string>> levels;
    while (!frontier.empty ())
      {
	levels.push_back (frontier);
	std::vector<std::string> next;
	for (const std::string &n : frontier)
	  {
	    for (const std::string &m : succ[n])
	      {
		if (--indeg[m] == 0)
		  {
		    next.push_back (m);
		  }
	      }
	  }
	std::sort (next.begin (), next.end ());
	frontier = next;
      }
    return levels;
  }
} // namespace

namespace cubimport
{

  bool
  is_partition_pseudo (const std::string &name)
  {
    return name.find ("__p__") != std::string::npos;
  }

  build_graph_status
  build_graph (const import_set &iset, bool skip_object_classes, dependency_graph &graph)
  {
    graph = dependency_graph ();
    graph.database_name = iset.database_name;

    /* Catalog reads run with authorization disabled so the DBA-group import
     * user can read underlying system classes (esp. _db_serial). */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    std::vector<std::vector<std::string>> class_rows, fk_rows, inh_rows, part_rows, serial_rows, obj_rows, idx_rows,
	idxkey_rows;

    int error = NO_ERROR;
    error = error ? error : collect_rows ("SELECT class_name FROM db_class "
					  "WHERE is_system_class='NO' AND class_type='CLASS' "
					  "ORDER BY class_name", 1, class_rows);
    error = error ? error : collect_rows ("SELECT table_name, referenced_table_name, constraint_name "
					  "FROM information_schema.referential_constraints", 3, fk_rows);
    error = error ? error : collect_rows ("SELECT class_name, super_class_name FROM db_direct_super_class",
					  2, inh_rows);
    error = error ? error : collect_rows ("SELECT class_name, partition_name FROM db_partition", 2, part_rows);
    error = error ? error : collect_rows ("SELECT name, class_name FROM _db_serial WHERE class_name IS NOT NULL",
					  2, serial_rows);
    error = error ? error : collect_rows ("SELECT class_name, attr_name, domain_class_name FROM db_attribute "
					  "WHERE data_type='OBJECT'", 3, obj_rows);
    error = error ? error : collect_rows ("SELECT class_name, index_name, is_primary_key, is_foreign_key, is_unique "
					  "FROM db_index", 5, idx_rows);
    /* Per-index key columns, in key order (key_order), for the FK/PK column
     * mapping the WU-33 anti-join needs. Captured here, at snapshot, while the
     * FK is still defined (WU-33 strips it before re-validating). */
    error = error ? error : collect_rows ("SELECT class_name, index_name, key_attr_name "
					  "FROM db_index_key ORDER BY class_name, index_name, key_order", 3, idxkey_rows);

    AU_RESTORE (au_save);

    if (error != NO_ERROR)
      {
	IMPORT_ERR (msg (IMPORTDB_MSG_GRAPH_QUERY_FAILED), iset.database_name.c_str (),
			       db_error_string (3));
	return build_graph_status::ERR_CATALOG;
      }

    /* user classes (already system/type-filtered + sorted; drop partition
     * pseudo-classes) */
    std::vector<std::string> classes;
    std::set<std::string> cset;
    for (const std::vector<std::string> &r : class_rows)
      {
	if (!is_partition_pseudo (r[0]))
	  {
	    classes.push_back (r[0]);
	    cset.insert (r[0]);
	  }
      }

    /* object columns (Q11) - a class with >=1 OBJECT attr is not CS-loadable */
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> obj_cols;
    for (const std::vector<std::string> &r : obj_rows)
      {
	if (cset.count (r[0]))
	  {
	    obj_cols[r[0]].push_back (std::make_pair (r[1], r[2]));
	  }
      }

    /* partitions */
    std::map<std::string, std::vector<std::string>> parts;
    for (const std::vector<std::string> &r : part_rows)
      {
	parts[r[0]].push_back (r[1]);
      }

    /* serials owned by a class (serial-ownership edge) */
    std::map<std::string, std::vector<std::string>> serials_by_class;
    for (const std::vector<std::string> &r : serial_rows)
      {
	if (cset.count (r[1]))
	  {
	    serials_by_class[r[1]].push_back (r[0]);
	  }
      }

    /* per-class constraint inventory (pk / uk / fk from db_index flags) */
    std::map<std::string, node_constraints> inv;
    for (const std::string &c : classes)
      {
	inv[c];
      }
    for (const std::vector<std::string> &r : idx_rows)
      {
	std::map<std::string, node_constraints>::iterator it = inv.find (r[0]);
	if (it == inv.end ())
	  {
	    continue;
	  }
	if (r[2] == "YES")
	  {
	    it->second.pk = r[1];
	  }
	else if (r[3] == "YES")
	  {
	    it->second.fk.push_back (r[1]);
	  }
	else if (r[4] == "YES")
	  {
	    it->second.unique.push_back (r[1]);
	  }
      }

    /* Q11 policy: reject-by-default, or skip + record under
     * --skip-object-classes. Determine the offending (not CS-loadable) set. */
    std::vector<std::string> offending;
    for (const std::string &c : classes)
      {
	if (obj_cols.count (c))
	  {
	    offending.push_back (c);
	  }
      }

    if (!offending.empty () && !skip_object_classes)
      {
	std::string detail;
	for (size_t i = 0; i < offending.size (); i++)
	  {
	    if (i != 0)
	      {
		detail += "; ";
	      }
	    detail += offending[i] + " (" + join_object_columns (obj_cols[offending[i]]) + ")";
	  }
	IMPORT_ERR (msg (IMPORTDB_MSG_OBJECT_CLASSES_REJECTED), detail.c_str ());
	return build_graph_status::ERR_OBJECT_CLASSES;
      }

    /* Q11 + SINGLE-layout limit: --skip-object-classes (we are here, so it is set)
     * excludes the object-valued classes from the graph, but a single-file object
     * dump interleaves their instances - carrying @oid object references - into
     * the one <prefix>_objects file, and CS-mode load cannot parse those
     * references (loaddb fails on the line regardless of its --ignore-class-file,
     * which filters inserts, not parsing). Such a file cannot be loaded in CS mode
     * at all, so reject early with the actionable workaround rather than failing
     * cryptically at the load phase. PER_CLASS dumps skip the whole file cleanly. */
    if (!offending.empty () && iset.object_kind == object_layout::SINGLE)
      {
	IMPORT_ERR (msg (IMPORTDB_MSG_OBJECT_CLASSES_SINGLE), iset.prefix.c_str (),
			       join_list (offending).c_str ());
	return build_graph_status::ERR_OBJECT_CLASSES;
      }

    std::set<std::string> skipped;
    if (!offending.empty ())
      {
	graph.skipped_classes = offending;	/* already sorted (classes is sorted) */
	skipped.insert (offending.begin (), offending.end ());
	IMPORT_PRINT (msg (IMPORTDB_MSG_OBJECT_CLASSES_SKIPPED), (int) offending.size (),
		 join_list (offending).c_str ());
      }

    /* present = user classes minus skipped */
    std::set<std::string> present;
    for (const std::string &c : classes)
      {
	if (!skipped.count (c))
	  {
	    present.insert (c);
	  }
      }

    /* nodes (present classes only, sorted order preserved) */
    for (const std::string &c : classes)
      {
	if (!present.count (c))
	  {
	    continue;
	  }
	graph_node node;
	node.name = c;
	std::map<std::string, std::vector<std::pair<std::string, std::string>>>::iterator oc = obj_cols.find (c);
	node.cs_loadable = (oc == obj_cols.end ());
	if (oc != obj_cols.end ())
	  {
	    node.object_columns = oc->second;
	  }
	std::map<std::string, std::vector<std::string>>::iterator p = parts.find (c);
	node.partitioned = (p != parts.end ());
	if (p != parts.end ())
	  {
	    node.partitions = p->second;
	  }
	node.constraints = inv[c];
	std::map<std::string, std::vector<std::string>>::iterator s = serials_by_class.find (c);
	if (s != serials_by_class.end ())
	  {
	    node.serials = s->second;
	  }
	graph.nodes.push_back (node);
      }

    /* index key columns keyed by "class\tindex", in key order (idxkey_rows is
     * already ORDER BY class_name, index_name, key_order, so appending in row
     * order preserves the key sequence). This is the source for each FK edge's
     * child FK columns and (via the parent's PK index name) the parent PK
     * columns - the WU-33 anti-join pairs them positionally. */
    std::map<std::string, std::vector<std::string>> key_columns;
    for (const std::vector<std::string> &r : idxkey_rows)
      {
	key_columns[r[0] + "\t" + r[1]].push_back (r[2]);
      }

    /* FK edges child -> parent, both ends present user classes. The column
     * vectors are captured from the snapshot catalog (FK still defined): the FK
     * index's own key columns (child_columns) paired with the parent PK index's
     * key columns (parent_pk_columns), plus the child's PK columns for row
     * identity in the WU-33 exceptions artifact (child_pk_columns). */
    for (const std::vector<std::string> &r : fk_rows)
      {
	if (present.count (r[0]) && present.count (r[1]))
	  {
	    fk_edge e;
	    e.child = r[0];
	    e.parent = r[1];
	    e.name = r[2];

	    std::map<std::string, std::vector<std::string>>::iterator fc = key_columns.find (r[0] + "\t" + r[2]);
	    if (fc != key_columns.end ())
	      {
		e.child_columns = fc->second;
	      }
	    const std::string &parent_pk = inv[r[1]].pk;
	    if (!parent_pk.empty ())
	      {
		std::map<std::string, std::vector<std::string>>::iterator pc = key_columns.find (r[1] + "\t" + parent_pk);
		if (pc != key_columns.end ())
		  {
		    e.parent_pk_columns = pc->second;
		  }
	      }
	    const std::string &child_pk = inv[r[0]].pk;
	    if (!child_pk.empty ())
	      {
		std::map<std::string, std::vector<std::string>>::iterator cpc = key_columns.find (r[0] + "\t" + child_pk);
		if (cpc != key_columns.end ())
		  {
		    e.child_pk_columns = cpc->second;
		  }
	      }
	    graph.fk_edges.push_back (e);
	  }
      }

    /* inheritance edges child -> super, drop partition pseudo rows */
    for (const std::vector<std::string> &r : inh_rows)
      {
	if (!is_partition_pseudo (r[0]) && present.count (r[0]) && present.count (r[1]))
	  {
	    inherit_edge e;
	    e.child = r[0];
	    e.super = r[1];
	    graph.inherit_edges.push_back (e);
	  }
      }

    /* cycles (DFS over FK) + level-set preview (Kahn layering) */
    graph.cycles = find_cycles (graph.fk_edges);
    std::vector<std::string> present_nodes (present.begin (), present.end ());
    graph.level_sets = level_sets (present_nodes, graph.fk_edges, graph.inherit_edges, graph.cycles);

    return build_graph_status::OK;
  }

  void
  print_graph_summary (const dependency_graph &graph)
  {
    size_t serial_count = 0;
    for (const graph_node &n : graph.nodes)
      {
	serial_count += n.serials.size ();
      }

    IMPORT_PRINT (msg (IMPORTDB_MSG_GRAPH_SUMMARY), graph.database_name.c_str (), (int) graph.nodes.size (),
	     (int) graph.fk_edges.size (), (int) graph.inherit_edges.size (), (int) serial_count);

    for (const graph_node &n : graph.nodes)
      {
	std::string cons = "pk=" + (n.constraints.pk.empty () ? std::string ("-") : n.constraints.pk)
			   + " uk=[" + join_list (n.constraints.unique) + "]"
			   + " fk=[" + join_list (n.constraints.fk) + "]";
	std::string tags;
	if (!n.cs_loadable)
	  {
	    tags += "NOT-CS-LOADABLE (" + join_object_columns (n.object_columns) + ")";
	  }
	if (n.partitioned)
	  {
	    if (!tags.empty ())
	      {
		tags += "; ";
	      }
	    tags += "partitioned [" + join_list (n.partitions) + "]";
	  }
	if (!n.serials.empty ())
	  {
	    if (!tags.empty ())
	      {
		tags += "; ";
	      }
	    tags += "serials=[" + join_list (n.serials) + "]";
	  }
	IMPORT_PRINT ("  - %s: %s%s%s%s\n", n.name.c_str (), cons.c_str (), tags.empty () ? "" : "   [",
		 tags.c_str (), tags.empty () ? "" : "]");
      }

    IMPORT_PRINT ("  FK edges (child -> parent):\n");
    if (graph.fk_edges.empty ())
      {
	IMPORT_PRINT ("      (none)\n");
      }
    for (const fk_edge &e : graph.fk_edges)
      {
	IMPORT_PRINT ("      %s -> %s   (%s)\n", e.child.c_str (), e.parent.c_str (), e.name.c_str ());
      }

    if (!graph.inherit_edges.empty ())
      {
	IMPORT_PRINT ("  inheritance (child -> super):\n");
	for (const inherit_edge &e : graph.inherit_edges)
	  {
	    IMPORT_PRINT ("      %s -> %s\n", e.child.c_str (), e.super.c_str ());
	  }
      }

    if (graph.cycles.empty ())
      {
	IMPORT_PRINT ("  cycles: none\n");
      }
    else
      {
	for (const std::vector<std::string> &cy : graph.cycles)
	  {
	    IMPORT_PRINT ("  cycle: [%s]\n", join_list (cy).c_str ());
	  }
      }

    size_t placed = 0;
    for (const std::vector<std::string> &lv : graph.level_sets)
      {
	placed += lv.size ();
      }
    IMPORT_PRINT ("  level sets (parallel-eligible; complete=%s):\n",
	     (placed == graph.nodes.size ()) ? "true" : "false");
    for (size_t i = 0; i < graph.level_sets.size (); i++)
      {
	IMPORT_PRINT ("      L%d: [%s]\n", (int) i, join_list (graph.level_sets[i]).c_str ());
      }
  }

} // namespace cubimport
