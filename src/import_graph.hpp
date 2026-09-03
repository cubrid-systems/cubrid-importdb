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
 * import_graph.hpp - Stage 2 (Graph builder) of the importdb pipeline
 *
 * After the Definition phase (WU-20) leaves a fully-defined empty target, the
 * Graph builder reads that DB's catalog and builds the in-memory
 * DependencyGraph (10-design.md §4): nodes (classes) with a CS-loadability flag
 * + a per-node constraint inventory, typed edges (FK child->parent, inheritance
 * child->super, serial-ownership class->serial), detected FK cycles, and a
 * parallel-eligible level-set preview (Kahn layering; inheritance is a hard
 * precedence, FK a soft one dropped on cycle). It runs on the SAME open session
 * as define (snapshot step) and reads only the public catalog via db_execute.
 *
 * Q11 (object-valued columns): a class with an OBJECT-domain attribute is not
 * CS-loadable. Default policy is reject-by-default (fail the run naming the
 * offending classes); with --skip-object-classes those classes are excluded
 * from the graph and recorded as skipped. The recipe is 11-design-detail.md §3
 * and the validated WU-03 prototype (m0/proto/wu03_graph.py).
 */

#ifndef _IMPORT_GRAPH_HPP_
#define _IMPORT_GRAPH_HPP_

#include "import_discovery.hpp"

#include <string>
#include <utility>
#include <vector>

namespace cubimport
{

  /*
   * Per-node constraint inventory - the strip/rebuild basis (10-design.md §6).
   * pk is the primary-key index name (empty when the class has none); unique
   * and fk are the standalone-UNIQUE and FK index names.
   */
  struct node_constraints
  {
    std::string pk;
    std::vector<std::string> unique;
    std::vector<std::string> fk;
  };

  /*
   * A graph node = one user class. cs_loadable is the Q11 gate (false iff the
   * class has an object-valued attribute); object_columns carries the offending
   * (attr, domain-class) pairs. partitioned/partitions and serials are
   * structural; constraints is the inventory above.
   */
  struct graph_node
  {
    std::string name;
    bool cs_loadable = true;
    std::vector<std::pair<std::string, std::string>> object_columns;
    bool partitioned = false;
    std::vector<std::string> partitions;
    node_constraints constraints;
    std::vector<std::string> serials;
  };

  /*
   * FK edge child -> parent (name = the FK constraint name). The column vectors
   * are captured at snapshot time (WU-21), while the FK is still defined, so the
   * WU-33 FK re-validation anti-join can pair the child's FK columns with the
   * parent's PK columns POSITIONALLY (FK col order <-> PK key order): FK column
   * names need not match the parent PK column names (e.g. lineorder.lo_custkey ->
   * customer.c_custkey), and a composite FK pairs several columns. child_columns
   * are the FK index's key columns in key order; parent_pk_columns are the
   * referenced parent's PK index key columns in key order (same arity as
   * child_columns); child_pk_columns are the child's own PK key columns (empty
   * when the child has no PK), used only to identify each offending row in the
   * WU-33 exceptions artifact. All three come from the catalog (db_index_key),
   * so the mapping is catalog-authoritative and layout-agnostic.
   */
  struct fk_edge
  {
    std::string child;
    std::string parent;
    std::string name;
    std::vector<std::string> child_columns;
    std::vector<std::string> parent_pk_columns;
    std::vector<std::string> child_pk_columns;
  };

  /* Inheritance edge child -> super. */
  struct inherit_edge
  {
    std::string child;
    std::string super;
  };

  /*
   * The DependencyGraph (10-design.md §4). nodes are sorted by class name;
   * cycles is the list of FK cycles (each a node list); level_sets is the
   * parallel-eligible layering preview; skipped_classes lists the Q11 classes
   * excluded under --skip-object-classes (empty otherwise).
   */
  struct dependency_graph
  {
    std::string database_name;
    std::vector<graph_node> nodes;
    std::vector<fk_edge> fk_edges;
    std::vector<inherit_edge> inherit_edges;
    std::vector<std::vector<std::string>> cycles;
    std::vector<std::vector<std::string>> level_sets;
    std::vector<std::string> skipped_classes;
  };

  /*
   * Graph-build outcome. OK means the graph was built (possibly with skipped
   * classes recorded); every other value corresponds to a named diagnostic
   * already emitted by build_graph ().
   */
  enum class build_graph_status
  {
    OK = 0,
    ERR_CATALOG,		/* a catalog query failed */
    ERR_OBJECT_CLASSES		/* Q11 reject: object-valued classes, no --skip-object-classes */
  };

  /*
   * Build the DependencyGraph from the connected target's catalog (assumes an
   * open CS-mode session - see import_session.hpp). On a catalog-read failure
   * returns ERR_CATALOG. On Q11: if any class is not CS-loadable and
   * skip_object_classes is false, emits the reject diagnostic and returns
   * ERR_OBJECT_CLASSES; if skip_object_classes is true, excludes those classes
   * from the graph, records them in graph.skipped_classes, reports them, and
   * returns OK with the remaining graph.
   */
  build_graph_status build_graph (const import_set &iset, bool skip_object_classes, dependency_graph &graph);

  /*
   * Print a concise summary of the graph to stdout (node count + edge counts,
   * per-node pk/uk/fk with NOT-CS-LOADABLE/partitioned/serials tags, FK and
   * inheritance edges, detected cycles, and the level-set preview) - modeled on
   * the WU-03 prototype's text output.
   */
  void print_graph_summary (const dependency_graph &graph);

  /* True for a partition pseudo-class -- the "<class>__p__<partition>" rows CUBRID
   * keeps in db_class. It is not a class an import defines, loads or refuses over,
   * so every inventory of "the target's user classes" has to drop it, which is why
   * this is not private to the graph builder. */
  bool is_partition_pseudo (const std::string &name);

} // namespace cubimport

#endif /* _IMPORT_GRAPH_HPP_ */
