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
 * import_plan.cpp - Stage 3 (Planner) of the importdb pipeline
 *
 * Turns the in-memory DependencyGraph (WU-21) into a Schedule (10-design.md §5):
 * the data-phase level sets (consumed from the graph) plus the terminal task
 * list with explicit ordering edges (§6 phase order). Pure computation on the
 * graph - no server calls, nothing executed. The terminal tasks are emitted in
 * phase order (rebuild PK/unique + plain indexes -> FK re-validate -> FK define
 * -> stats -> define triggers), which is a valid topological order by
 * construction; validate_schedule () machine-checks that independently (acyclic
 * task graph + emitted order respects every prerequisite edge).
 */

#include "import_plan.hpp"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }

  /* Comma-separated join of a string list (level sets, diagnostics). */
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

  /* The uppercase label for a terminal task kind (used in the print + reasons). */
  const char *
  task_kind_name (cubimport::terminal_task_kind kind)
  {
    switch (kind)
      {
      case cubimport::terminal_task_kind::REBUILD_PK:
	return "REBUILD_PK";
      case cubimport::terminal_task_kind::REBUILD_UNIQUE:
	return "REBUILD_UNIQUE";
      case cubimport::terminal_task_kind::BUILD_INDEX:
	return "BUILD_INDEX";
      case cubimport::terminal_task_kind::FK_VALIDATE:
	return "FK_VALIDATE";
      case cubimport::terminal_task_kind::FK_DEFINE:
	return "FK_DEFINE";
      case cubimport::terminal_task_kind::STATS:
	return "STATS";
      case cubimport::terminal_task_kind::DEFINE_TRIGGERS:
	return "DEFINE_TRIGGERS";
      }
    return "?";
  }

  /* The human-readable target of a task ("cls (name)", "child -> parent (name)",
   * etc.) - the right-hand side of a print line and of a machine-check reason. */
  std::string
  task_target (const cubimport::terminal_task &t)
  {
    switch (t.kind)
      {
      case cubimport::terminal_task_kind::FK_VALIDATE:
      case cubimport::terminal_task_kind::FK_DEFINE:
	return t.cls + " -> " + t.parent + " (" + t.name + ")";
      case cubimport::terminal_task_kind::BUILD_INDEX:
	return "<deferred indexes: " + t.name + ">";
      case cubimport::terminal_task_kind::DEFINE_TRIGGERS:
	return "<deferred triggers: " + t.name + ">";
      case cubimport::terminal_task_kind::STATS:
	return t.cls;
      default:			/* REBUILD_PK / REBUILD_UNIQUE */
	return t.cls + " (" + t.name + ")";
      }
  }

  /* A compact label for one task, used inside machine-check reason messages. */
  std::string
  task_label (const cubimport::terminal_task &t)
  {
    return std::string (task_kind_name (t.kind)) + " " + task_target (t);
  }
} // namespace

namespace cubimport
{

  build_schedule_status
  build_schedule (const dependency_graph &graph, const import_set &iset, schedule &sched)
  {
    sched = schedule ();
    sched.database_name = graph.database_name;

    /* data phase: the parallel-eligible level sets already computed by the graph
     * (inheritance/serial precedence bounded; soft FK dropped on cycle, Q2). */
    sched.data_levels = graph.level_sets;

    std::vector<terminal_task> &tasks = sched.terminal_tasks;

    /* wiring maps: a class's key-rebuild task indices, so FK/STATS tasks can name
     * their prerequisites. */
    std::map<std::string, std::size_t> pk_task;
    std::map<std::string, std::vector<std::size_t>> uk_tasks;
    bool have_index = false;
    std::size_t index_task = 0;

    /* phase 1a: REBUILD_PK per class that has a PK (graph.nodes is name-sorted). */
    for (const graph_node &n : graph.nodes)
      {
	if (!n.constraints.pk.empty ())
	  {
	    terminal_task t;
	    t.kind = terminal_task_kind::REBUILD_PK;
	    t.cls = n.name;
	    t.name = n.constraints.pk;
	    pk_task[n.name] = tasks.size ();
	    tasks.push_back (t);
	  }
      }

    /* phase 1b: REBUILD_UNIQUE per standalone-unique constraint. */
    for (const graph_node &n : graph.nodes)
      {
	for (const std::string &uk : n.constraints.unique)
	  {
	    terminal_task t;
	    t.kind = terminal_task_kind::REBUILD_UNIQUE;
	    t.cls = n.name;
	    t.name = uk;
	    uk_tasks[n.name].push_back (tasks.size ());
	    tasks.push_back (t);
	  }
      }

    /* phase 1c: BUILD_INDEX - one task for the deferred plain-index file (the
     * <prefix>_indexes CREATE INDEXes); per-file granularity, no parse needed. */
    if (!iset.index_file.empty ())
      {
	terminal_task t;
	t.kind = terminal_task_kind::BUILD_INDEX;
	t.name = iset.index_file;
	have_index = true;
	index_task = tasks.size ();
	tasks.push_back (t);
      }

    /* A parent's key-rebuild tasks (its PK + all its uniques): the anti-join and
     * the FK define both need the parent key index, and we cannot tell from the
     * graph which key an FK references, so we depend on every one of them. */
    std::map<std::string, std::vector<std::size_t>> parent_keys_cache;
    for (std::map<std::string, std::size_t>::iterator it = pk_task.begin (); it != pk_task.end (); ++it)
      {
	parent_keys_cache[it->first].push_back (it->second);
      }
    for (std::map<std::string, std::vector<std::size_t>>::iterator it = uk_tasks.begin (); it != uk_tasks.end (); ++it)
      {
	std::vector<std::size_t> &v = parent_keys_cache[it->first];
	v.insert (v.end (), it->second.begin (), it->second.end ());
      }

    /* phase 2: FK_VALIDATE per edge, gated on the parent's key rebuilds. */
    std::vector<std::size_t> validate_task (graph.fk_edges.size ());
    for (std::size_t i = 0; i < graph.fk_edges.size (); i++)
      {
	const fk_edge &e = graph.fk_edges[i];
	terminal_task t;
	t.kind = terminal_task_kind::FK_VALIDATE;
	t.cls = e.child;
	t.parent = e.parent;
	t.name = e.name;
	std::map<std::string, std::vector<std::size_t>>::iterator pk = parent_keys_cache.find (e.parent);
	if (pk != parent_keys_cache.end ())
	  {
	    t.prereqs = pk->second;
	  }
	validate_task[i] = tasks.size ();
	tasks.push_back (t);
      }

    /* phase 3: FK_DEFINE per edge, after that edge's FK_VALIDATE (define-after-
     * validate, Q10) and the same parent-key rebuilds. */
    for (std::size_t i = 0; i < graph.fk_edges.size (); i++)
      {
	const fk_edge &e = graph.fk_edges[i];
	terminal_task t;
	t.kind = terminal_task_kind::FK_DEFINE;
	t.cls = e.child;
	t.parent = e.parent;
	t.name = e.name;
	std::map<std::string, std::vector<std::size_t>>::iterator pk = parent_keys_cache.find (e.parent);
	if (pk != parent_keys_cache.end ())
	  {
	    t.prereqs = pk->second;
	  }
	t.prereqs.push_back (validate_task[i]);
	tasks.push_back (t);
      }

    /* phase 4: STATS per class, after that class's rebuilds and the index build. */
    for (const graph_node &n : graph.nodes)
      {
	terminal_task t;
	t.kind = terminal_task_kind::STATS;
	t.cls = n.name;
	std::map<std::string, std::vector<std::size_t>>::iterator pk = parent_keys_cache.find (n.name);
	if (pk != parent_keys_cache.end ())
	  {
	    t.prereqs = pk->second;
	  }
	if (have_index)
	  {
	    t.prereqs.push_back (index_task);
	  }
	tasks.push_back (t);
      }

    /* phase 5: DEFINE_TRIGGERS - one task, strictly last (after every other
     * task), present only when the dump carries a trigger file. */
    if (!iset.trigger_file.empty ())
      {
	terminal_task t;
	t.kind = terminal_task_kind::DEFINE_TRIGGERS;
	t.name = iset.trigger_file;
	for (std::size_t i = 0; i < tasks.size (); i++)
	  {
	    t.prereqs.push_back (i);
	  }
	tasks.push_back (t);
      }

    /* machine-check: a produced Schedule that violates its own ordering is a
     * logic bug (validate_schedule emits the diagnostic). */
    if (!validate_schedule (sched))
      {
	return build_schedule_status::ERR_INVALID_SCHEDULE;
      }
    return build_schedule_status::OK;
  }

  bool
  validate_schedule (const schedule &sched)
  {
    const std::vector<terminal_task> &tasks = sched.terminal_tasks;
    const std::size_t n = tasks.size ();

    /* Check 1 - emitted order respects every prerequisite edge: a prerequisite
     * must be in range and emitted strictly before the task that needs it. */
    for (std::size_t i = 0; i < n; i++)
      {
	for (std::size_t p : tasks[i].prereqs)
	  {
	    if (p >= n || p >= i)
	      {
		std::string reason = "task #" + std::to_string (i) + " (" + task_label (tasks[i])
				     + ") is emitted before its prerequisite #" + std::to_string (p);
		PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_SCHEDULE_INVALID), sched.database_name.c_str (),
				       reason.c_str ());
		return false;
	      }
	  }
      }

    /* Check 2 - the task dependency graph is acyclic (a topological order
     * exists): Kahn over the prerequisite edges (prereq -> task). */
    std::vector<std::vector<std::size_t>> succ (n);
    std::vector<int> indeg (n, 0);
    for (std::size_t i = 0; i < n; i++)
      {
	for (std::size_t p : tasks[i].prereqs)
	  {
	    succ[p].push_back (i);
	    indeg[i]++;
	  }
      }

    std::vector<std::size_t> order;
    order.reserve (n);
    for (std::size_t i = 0; i < n; i++)
      {
	if (indeg[i] == 0)
	  {
	    order.push_back (i);
	  }
      }
    for (std::size_t h = 0; h < order.size (); h++)
      {
	for (std::size_t v : succ[order[h]])
	  {
	    if (--indeg[v] == 0)
	      {
		order.push_back (v);
	      }
	  }
      }
    if (order.size () != n)
      {
	std::string reason = "the terminal task dependency graph has a cycle (topological order does not exist)";
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_SCHEDULE_INVALID), sched.database_name.c_str (), reason.c_str ());
	return false;
      }

    return true;
  }

  void
  print_schedule (const schedule &sched)
  {
    std::size_t placed = 0;
    for (const std::vector<std::string> &lv : sched.data_levels)
      {
	placed += lv.size ();
      }

    fprintf (stdout, msg (IMPORTDB_MSG_SCHEDULE_SUMMARY), sched.database_name.c_str (),
	     (int) sched.data_levels.size (), (int) sched.terminal_tasks.size ());

    fprintf (stdout, "  data phase (parallel-eligible level sets):\n");
    for (std::size_t i = 0; i < sched.data_levels.size (); i++)
      {
	fprintf (stdout, "      L%d: [%s]\n", (int) i, join_list (sched.data_levels[i]).c_str ());
      }

    fprintf (stdout, "  terminal tasks (in a valid execution order):\n");
    if (sched.terminal_tasks.empty ())
      {
	fprintf (stdout, "      (none)\n");
      }
    for (std::size_t i = 0; i < sched.terminal_tasks.size (); i++)
      {
	const terminal_task &t = sched.terminal_tasks[i];
	std::string deps;
	for (std::size_t k = 0; k < t.prereqs.size (); k++)
	  {
	    if (k != 0)
	      {
		deps += ", ";
	      }
	    deps += "#" + std::to_string (t.prereqs[k]);
	  }
	fprintf (stdout, "      #%-2d %-15s %s%s%s%s\n", (int) i, task_kind_name (t.kind), task_target (t).c_str (),
		 deps.empty () ? "" : "   [after ", deps.c_str (), deps.empty () ? "" : "]");
      }
  }

} // namespace cubimport
