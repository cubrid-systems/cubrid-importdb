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
 * import_strip.cpp - Stage 4 (Strip phase, step 1 of the constraint lifecycle)
 *
 * Generates the constraint-DROP DDL from the in-memory DependencyGraph and runs
 * it on the connected empty target so the data phase loads into bare heaps
 * (10-design.md §6). This productizes the validated WU-04 prototype's
 * strip_ddl() (m0/proto/wu04_strip_rebuild.py): the same statements
 *   ALTER TABLE [<child>] DROP FOREIGN KEY [<fk>];
 *   ALTER TABLE [<class>] DROP CONSTRAINT [<uk>];
 *   ALTER TABLE [<class>] DROP PRIMARY KEY;
 * in the §4.1 global order (all FKs, then all standalone uniques, then all PKs),
 * ported to C++ over the same public single-statement primitive the Graph
 * builder reads the catalog with (db_execute). A constraint is dropped only on
 * the class that OWNS it: CUBRID shares a superclass constraint (its name) with
 * every subclass, so an inherited copy is skipped - the owner's drop removes it
 * from the whole inheritance chain (partinh is the crux fixture). As define
 * does for its DDL,
 * authorization is disabled around execution for the DBA-group path. The caller
 * owns the transaction (import_session.hpp): strip () neither commits nor aborts;
 * on success the caller commits the bare-heap target, on failure it aborts.
 * loaddb is not touched.
 */

#include "import_strip.hpp"
#include "import_progress.hpp"
#include "import_resume.hpp"

#include "db.h"
#include "authenticate.h"
#include "error_manager.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  /*
   * Compile + execute one generated DROP statement string on the open session
   * (the same public single-statement primitive the Graph builder uses for its
   * catalog reads). Returns NO_ERROR, or the negative db_execute error code
   * after emitting the strip diagnostic naming the failing statement.
   */
  int
  exec_strip_stmt (const std::string &stmt)
  {
    DB_QUERY_RESULT *result = NULL;
    DB_QUERY_ERROR query_error;

    int error = db_execute (stmt.c_str (), &result, &query_error);
    if (error < 0)
      {
	IMPORT_ERR (msg (IMPORTDB_MSG_STRIP_STMT_FAILED), stmt.c_str (), db_error_string (3));
	return error;
      }
    db_query_end (result);
    return NO_ERROR;
  }

  /* True when node n lists name among its pk/unique/fk inventory. */
  bool
  node_has_constraint (const cubimport::graph_node &n, const std::string &name)
  {
    if (n.constraints.pk == name)
      {
	return true;
      }
    for (const std::string &u : n.constraints.unique)
      {
	if (u == name)
	  {
	    return true;
	  }
      }
    for (const std::string &f : n.constraints.fk)
      {
	if (f == name)
	  {
	    return true;
	  }
      }
    return false;
  }

  /*
   * True when constraint name on class cls is inherited from a superclass.
   * CUBRID keeps the defining class's constraint/index name on every subclass,
   * so an inherited PK/UK/FK appears in the subclass's catalog inventory under
   * the same name. Such a constraint is owned by - and must be dropped on - the
   * defining superclass only: dropping it again on the subclass would fail
   * ("constraint not found") because the owner's drop already removed it from
   * the whole inheritance chain. Walks the transitive superclass chain built
   * from the graph's inheritance edges.
   */
  bool
  is_inherited_constraint (const std::map<std::string, const cubimport::graph_node *> &by_name,
			   const std::map<std::string, std::vector<std::string>> &supers, const std::string &cls,
			   const std::string &name)
  {
    std::vector<std::string> pending;
    std::map<std::string, std::vector<std::string>>::const_iterator sit = supers.find (cls);
    if (sit != supers.end ())
      {
	pending = sit->second;
      }

    std::set<std::string> seen;
    while (!pending.empty ())
      {
	std::string super = pending.back ();
	pending.pop_back ();
	if (!seen.insert (super).second)
	  {
	    continue;
	  }
	std::map<std::string, const cubimport::graph_node *>::const_iterator nit = by_name.find (super);
	if (nit != by_name.end () && nit->second != NULL && node_has_constraint (*nit->second, name))
	  {
	    return true;
	  }
	std::map<std::string, std::vector<std::string>>::const_iterator ssit = supers.find (super);
	if (ssit != supers.end ())
	  {
	    for (const std::string &s : ssit->second)
	      {
		pending.push_back (s);
	      }
	  }
      }
    return false;
  }

  /*
   * Generate + execute the constraint-DROP DDL for graph in the §4.1 global
   * order (all FKs, then all standalone uniques, then all PKs), appending each
   * dropped constraint to stripped in drop order. When present is non-null (a
   * resumed run) a constraint the catalog no longer holds was already dropped
   * by the interrupted run: it is recorded but not dropped again. A constraint is dropped only
   * on the class that OWNS it: an inherited copy on a subclass is skipped, since
   * the owner's drop removes it from the whole inheritance chain. Stops at the
   * first failure and returns its error code (the diagnostic is already
   * emitted); the graph's nodes are pre-sorted by class name, so the order is
   * deterministic.
   */
  int
  run_strip_ddl (const cubimport::dependency_graph &graph, std::vector<cubimport::stripped_constraint> &stripped,
		 const cubimport::catalog_state *present)
  {
    std::map<std::string, const cubimport::graph_node *> by_name;
    for (const cubimport::graph_node &n : graph.nodes)
      {
	by_name[n.name] = &n;
      }
    std::map<std::string, std::vector<std::string>> supers;
    for (const cubimport::inherit_edge &e : graph.inherit_edges)
      {
	supers[e.child].push_back (e.super);
      }

    for (const cubimport::graph_node &n : graph.nodes)
      {
	for (const std::string &fk : n.constraints.fk)
	  {
	    if (is_inherited_constraint (by_name, supers, n.name, fk))
	      {
		continue;
	      }
	    if (present == NULL || present->has_index (n.name, fk))
	      {
		int error = exec_strip_stmt ("ALTER TABLE [" + n.name + "] DROP FOREIGN KEY [" + fk + "];");
		if (error != NO_ERROR)
		  {
		    return error;
		  }
	      }
	    stripped.push_back ({ cubimport::stripped_kind::FK, n.name, fk });
	  }
      }
    for (const cubimport::graph_node &n : graph.nodes)
      {
	for (const std::string &uk : n.constraints.unique)
	  {
	    if (is_inherited_constraint (by_name, supers, n.name, uk))
	      {
		continue;
	      }
	    if (present == NULL || present->has_index (n.name, uk))
	      {
		int error = exec_strip_stmt ("ALTER TABLE [" + n.name + "] DROP CONSTRAINT [" + uk + "];");
		if (error != NO_ERROR)
		  {
		    return error;
		  }
	      }
	    stripped.push_back ({ cubimport::stripped_kind::UNIQUE, n.name, uk });
	  }
      }
    for (const cubimport::graph_node &n : graph.nodes)
      {
	if (!n.constraints.pk.empty () && !is_inherited_constraint (by_name, supers, n.name, n.constraints.pk))
	  {
	    if (present == NULL || present->has_index (n.name, n.constraints.pk))
	      {
		int error = exec_strip_stmt ("ALTER TABLE [" + n.name + "] DROP PRIMARY KEY;");
		if (error != NO_ERROR)
		  {
		    return error;
		  }
	      }
	    stripped.push_back ({ cubimport::stripped_kind::PK, n.name, n.constraints.pk });
	  }
      }
    return NO_ERROR;
  }
} // namespace

namespace cubimport
{

  strip_status
  strip (const dependency_graph &graph, std::vector<stripped_constraint> &stripped, const catalog_state *present)
  {
    /* As define does for its DDL, DBA-group members run the strip DDL with
     * authorization disabled. */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);
    int error = run_strip_ddl (graph, stripped, present);
    AU_RESTORE (au_save);

    if (error != NO_ERROR)
      {
	/* The caller's session_close (false) aborts the partial strip. */
	stripped.clear ();
	return strip_status::ERR_STRIP;
      }

    IMPORT_PRINT (msg (IMPORTDB_MSG_STRIP_COMPLETE), (int) stripped.size (), graph.database_name.c_str ());
    return strip_status::OK;
  }

} // namespace cubimport
