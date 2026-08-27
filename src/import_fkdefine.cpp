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
 * import_fkdefine.cpp - Stage 4 (FK define, step 5 of the constraint lifecycle)
 *
 * Defines the FK on every validated-clean edge of the loaded, rebuilt, validated
 * target - the terminal step of the §6 constraint lifecycle (10-design.md §6 /
 * Q10). It re-executes the dump's OWN FK ADD statements verbatim (mirroring the
 * Rebuild phase's PK/UK re-execution): for a SPLIT dump the isolated
 * <prefix>_schema_fk file(s) named in iset.schema_apply_order, for a DEFAULT dump
 * the ALTER CLASS ... ADD CONSTRAINT [<name>] FOREIGN KEY statements isolated from
 * the single <prefix>_schema by matching the graph's FK edge names (the FK form
 * ADD CONSTRAINT ... FOREIGN KEY is distinct from the PK/UK form ADD ATTRIBUTE
 * CONSTRAINT ... PRIMARY KEY|UNIQUE, so PK/UK statements are never picked up).
 * Statements run one at a time over the same public single-statement primitive the
 * Strip/Rebuild phases use (db_execute) on the already-open session, authorization
 * disabled for the DBA-group path; on a populated (validated-clean) table an
 * ADD ... FOREIGN KEY bulk-builds the FK b-tree bottom-up server-side. loaddb is
 * not touched.
 *
 * An edge is WITHHELD (its FK not defined) when the Rebuild phase withheld it
 * (rebuild.withheld - parent PK un-rebuilt), the FK re-validation phase found it
 * violated (validate - orphan rows), or (under fail-fast) re-validation never
 * reached it. A withheld edge's FK is skipped and its exact re-add DDL recorded in
 * summary.withheld and appended to the exceptions artifact so an operator can add
 * it after repairing the data. Because the FK is defined only on data already
 * proven clean, FM5 (a defined FK trusting orphan rows) cannot occur.
 *
 * FK-define contract (10-design.md §6 / Q10): all-clean -> OK (catalog ==
 * snapshot, the full round-trip). Any withheld edge -> PARTIAL (the clean edges'
 * FK is defined + committed, the withheld edges recorded, the run exits non-zero).
 * A hard error defining a CLEAN edge's FK (unexpected - the data validated clean),
 * a source file that cannot be opened, or an exceptions-write failure is
 * ERR_FKDEFINE - the caller aborts. The caller owns the transaction
 * (import_session.hpp): define_fks () neither commits nor aborts.
 */

#include "import_fkdefine.hpp"
#include "import_validate.hpp"	/* enumerate_fk_orphans / write_fk_exceptions */
#include "import_resume.hpp"

#include "db.h"
#include "authenticate.h"
#include "error_manager.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
  /* Basename of the importdb-written exceptions artifact inside the importdb
   * directory - the same file the FK re-validation phase (WU-33) writes its
   * orphan rows to. The FK define phase appends a clearly-labeled section listing
   * the withheld FK edges + their re-add DDL, creating the file if WU-33 found no
   * violation (a withheld edge can also come from a Rebuild-phase withhold). */
  const char *const EXCEPTIONS_BASENAME = "importdb.exceptions";

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

  /* True when name ends with suffix (used to pick the split FK file out of the
   * schema apply order). */
  bool
  ends_with (const std::string &name, const std::string &suffix)
  {
    return name.size () >= suffix.size () && name.compare (name.size () - suffix.size (), suffix.size (), suffix) == 0;
  }

  /* Read the whole text of a DDL file. Returns NO_ERROR, or ER_GENERIC_ERROR
   * (leaving errno set) when the file cannot be opened. */
  int
  read_file_text (const std::string &path, std::string &out)
  {
    FILE *fp = fopen (path.c_str (), "r");
    if (fp == NULL)
      {
	return ER_GENERIC_ERROR;
      }
    char buf[4096];
    size_t n;
    while ((n = fread (buf, 1, sizeof (buf), fp)) > 0)
      {
	out.append (buf, n);
      }
    fclose (fp);
    return NO_ERROR;
  }

  std::string
  to_upper (const std::string &s)
  {
    std::string u = s;
    for (char &c : u)
      {
	c = (char) toupper ((unsigned char) c);
      }
    return u;
  }

  std::string
  trim (const std::string &s)
  {
    const char *ws = " \t\r\n";
    size_t b = s.find_first_not_of (ws);
    if (b == std::string::npos)
      {
	return "";
      }
    size_t e = s.find_last_not_of (ws);
    return s.substr (b, e - b + 1);
  }

  /* Split a DDL file's text into statements on the semicolon terminator (FK ADD
   * statements carry no embedded semicolons or strings), trimming each and
   * dropping empties. */
  std::vector<std::string>
  split_statements (const std::string &text)
  {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size ())
      {
	size_t semi = text.find (';', start);
	std::string stmt = (semi == std::string::npos) ? text.substr (start) : text.substr (start, semi - start);
	std::string t = trim (stmt);
	if (!t.empty ())
	  {
	    out.push_back (t);
	  }
	if (semi == std::string::npos)
	  {
	    break;
	  }
	start = semi + 1;
      }
    return out;
  }

  /* Extract every constraint name from a statement's "CONSTRAINT [<name>]"
   * clauses (case-insensitive on the keyword; the bracketed name keeps its
   * original case). An FK ADD statement carries exactly one. */
  std::vector<std::string>
  constraint_names (const std::string &stmt)
  {
    std::vector<std::string> names;
    const std::string upper = to_upper (stmt);
    const std::string kw = "CONSTRAINT";
    size_t pos = 0;
    while ((pos = upper.find (kw, pos)) != std::string::npos)
      {
	size_t lb = stmt.find ('[', pos + kw.size ());
	if (lb == std::string::npos)
	  {
	    break;
	  }
	size_t rb = stmt.find (']', lb + 1);
	if (rb == std::string::npos)
	  {
	    break;
	  }
	names.push_back (stmt.substr (lb + 1, rb - lb - 1));
	pos = rb + 1;
      }
    return names;
  }

  /*
   * Compile + execute one statement on the open session (the same public
   * single-statement primitive the Strip/Rebuild phases use). Returns NO_ERROR,
   * or the negative db_execute error code - the caller captures db_error_string ()
   * and reports the clean-edge define failure as a hard error.
   */
  int
  exec_stmt (const std::string &stmt)
  {
    DB_QUERY_RESULT *result = NULL;
    DB_QUERY_ERROR query_error;

    int error = db_execute (stmt.c_str (), &result, &query_error);
    if (error < 0)
      {
	return error;
      }
    db_query_end (result);
    return NO_ERROR;
  }

  /* The line the FK-withheld block starts with. It is also the marker this phase
   * owns: everything from it to the end of the artifact belongs to the FK define
   * phase and is REPLACED on every run, so a resumed run does not stack a second
   * copy of the same repair records on top of the first. (The FK re-validation
   * phase truncates the artifact with "w" before writing its orphan rows, so the
   * two phases together always leave exactly one current copy of each.) */
  const char *const FK_SECTION_MARKER =
	  "# CUBRID importdb FK define: the following FK(s) were NOT defined (withheld).";

  /* Write the FK-withheld section into the exceptions artifact, after any WU-33
   * orphan records and in place of any FK section a previous attempt left there
   * (creating the file when there was none). Returns true on success; on failure
   * leaves errno set for the caller's diagnostic. */
  bool
  write_withheld_exceptions (const std::string &path, const cubimport::fkdefine_summary &summary)
  {
    /* Keep whatever precedes this phase's own marker; drop the rest. A resumed
     * FK define recomputes the identical withheld set - the guard only skips
     * FKs that are DEFINED, and a withheld one never is - so appending would
     * duplicate every record, in the one artifact the operator repairs from. */
    std::string kept;
    {
      std::ifstream in (path.c_str ());
      if (in.is_open ())
	{
	  std::string all ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char> ());
	  const size_t marker = all.find (FK_SECTION_MARKER);
	  kept = (marker == std::string::npos) ? all : all.substr (0, marker);
	}
    }

    std::string content = kept;
    content += FK_SECTION_MARKER;
    content += "\n";
    content += "# An operator repairs the referenced data, then re-runs each 'readd' statement to add the FK.\n";
    for (const cubimport::withheld_define &w : summary.withheld)
      {
	content += "[fk-withheld] " + w.child + " -> " + w.parent + " (" + w.name + ") :: " + w.reason + "\n";
	content += "readd: " + w.readd_ddl + "\n";
      }
    content += "\n";

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

  fkdefine_status
  define_fks (const import_set &iset, const dependency_graph &graph, const rebuild_summary &rebuild,
	      bool continue_on_error, validate_summary &validate, fkdefine_summary &summary,
	      const catalog_state *present)
  {
    summary = fkdefine_summary ();

    /* Every FK edge keyed by its constraint name: this maps a matched statement
     * back to its child/parent and is the authority for whether a statement is an
     * FK define target at all (a <prefix>_schema statement whose constraint name
     * is not here is a PK/UK/other and is skipped). */
    std::map<std::string, const fk_edge *> fk_by_name;
    for (const fk_edge &e : graph.fk_edges)
      {
	fk_by_name[e.name] = &e;
      }

    /* An edge whose parent PK the Rebuild phase withheld cannot be defined (no
     * parent PK index to reference); keyed by FK name with its rebuild reason. */
    std::map<std::string, std::string> withheld_by_rebuild;
    for (const withheld_fk &w : rebuild.withheld)
      {
	withheld_by_rebuild[w.name] = w.reason;
      }

    /* No prior validation to read. The engine validates as it builds: this phase
     * ATTEMPTS each edge's ADD CONSTRAINT and reads the verdict from the error
     * code. `violated` fills in as that happens, and drives the withheld records
     * and the exceptions artifact exactly as a separate phase's results used to.
     *
     * Under fail-fast the first rejected edge stops further attempts; the edges
     * after it are withheld as un-attempted rather than silently defined. */
    std::map<std::string, int64_t> violated;
    bool stop_attempting = false;

    validate = validate_summary ();

    /* The FK statement sources, mirroring the Rebuild phase's PK/UK sourcing.
     * SPLIT runs the isolated <prefix>_schema_fk file(s) from the apply order;
     * DEFAULT runs the single interleaved <prefix>_schema, from which the FK-name
     * filter isolates just the ADD CONSTRAINT ... FOREIGN KEY statements. */
    std::vector<std::string> fk_files;
    if (iset.schema_kind == schema_layout::SPLIT)
      {
	for (const std::string &f : iset.schema_apply_order)
	  {
	    if (ends_with (f, "_schema_fk"))
	      {
		fk_files.push_back (f);
	      }
	  }
      }
    else
      {
	fk_files.push_back (iset.schema_file);
      }

    /* As the other server-side phases do, DBA-group members run the FK define DDL
     * with authorization disabled. */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    bool hard_error = false;
    std::set<std::string> processed;

    for (const std::string &f : fk_files)
      {
	std::string text;
	const std::string full = path_join (iset.dump_dir, f);
	if (read_file_text (full, text) != NO_ERROR)
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_FKDEFINE_FILE_OPEN_FAILED), full.c_str (), strerror (errno));
	    hard_error = true;
	    break;
	  }

	for (const std::string &stmt : split_statements (text))
	  {
	    /* Match this statement's constraint clause against the graph's FK
	     * names; a statement with no matched FK name is not an FK define
	     * target (CREATE CLASS / column / PK/UK / COMMIT WORK) and is skipped. */
	    const fk_edge *edge = NULL;
	    for (const std::string &nm : constraint_names (stmt))
	      {
		std::map<std::string, const fk_edge *>::iterator it = fk_by_name.find (nm);
		if (it != fk_by_name.end ())
		  {
		    edge = it->second;
		    break;
		  }
	      }
	    if (edge == NULL || !processed.insert (edge->name).second)
	      {
		continue;
	      }

	    /* Decide clean-vs-withheld: define only an edge with its parent PK
	     * present (not Rebuild-withheld) that FK re-validation proved clean;
	     * otherwise withhold and record the exact re-add DDL. */
	    std::map<std::string, std::string>::iterator rw = withheld_by_rebuild.find (edge->name);
	    if (rw == withheld_by_rebuild.end () && !stop_attempting)
	      {
		/* WU-50 resume guard: an interrupted prior define may already
		 * have defined this FK. Re-executing the ADD would fail on
		 * "already exists" - which this phase reads as the impossible
		 * "clean data, failed FK build" and aborts the run. */
		if (present != NULL && present->has_index (edge->child, edge->name))
		  {
		    summary.defined.push_back ({ edge->child, edge->parent, edge->name });
		    continue;
		  }

		int error = exec_stmt (stmt + ";");
		if (error == NO_ERROR)
		  {
		    summary.defined.push_back ({ edge->child, edge->parent, edge->name });
		    fk_edge_result r;
		    r.child = edge->child;
		    r.parent = edge->parent;
		    r.name = edge->name;
		    r.orphans = 0;
		    validate.edges.push_back (r);
		    validate.validated_edges++;
		  }
		else if (db_error_code () == ER_FK_INVALID)
		  {
		    /* The engine built the FK's b-tree over the loaded rows and
		     * btree_load_check_fk found a key with no parent -- so the data
		     * is violated and no FK was created (no catalog residue, and the
		     * transaction is unharmed; both measured). The engine names only
		     * the FIRST offending value, so enumerate the rest here, for this
		     * edge alone. */
		    std::vector<fk_orphan> orphans;
		    if (enumerate_fk_orphans (*edge, orphans) != NO_ERROR)
		      {
			/* the enumeration itself failed: already reported there */
			hard_error = true;
			break;
		      }
		    const int64_t n = (int64_t) orphans.size ();
		    violated[edge->name] = n;
		    validate.orphans.insert (validate.orphans.end (), orphans.begin (), orphans.end ());
		    fk_edge_result r;
		    r.child = edge->child;
		    r.parent = edge->parent;
		    r.name = edge->name;
		    r.orphans = n;
		    validate.edges.push_back (r);
		    validate.validated_edges++;
		    validate.violated_edges++;
		    validate.total_orphans += n;

		    /* four arguments, matching message 41 (the name appears twice in
		     * the text but is one argument). */
		    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_VALIDATE_EDGE_VIOLATED), (int) n, edge->child.c_str (),
					   edge->parent.c_str (), edge->name.c_str ());

		    withheld_define w;
		    w.child = edge->child;
		    w.parent = edge->parent;
		    w.name = edge->name;
		    w.reason = "ADD FOREIGN KEY rejected by the engine: " + std::to_string (n) + " orphan row(s)";
		    w.readd_ddl = stmt + ";";
		    summary.withheld.push_back (w);

		    /* Default policy is fail-fast: attempt no further edges. */
		    if (!continue_on_error)
		      {
			stop_attempting = true;
		      }
		    continue;
		  }
		else
		  {
		    /* Any other failure is a real error -- the FK could not be built
		     * for a reason that is not the data. Abort the run. */
		    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_FKDEFINE_FAILED), edge->name.c_str (), edge->child.c_str (),
					   edge->parent.c_str (), db_error_string (3));
		    hard_error = true;
		    break;
		  }
	      }
	    else
	      {
		withheld_define w;
		w.child = edge->child;
		w.parent = edge->parent;
		w.name = edge->name;
		if (rw != withheld_by_rebuild.end ())
		  {
		    w.reason = "parent key withheld by rebuild (" + rw->second + ")";
		  }
		else
		  {
		    w.reason = "not attempted (fail-fast stopped at an earlier rejected edge)";
		  }
		w.readd_ddl = stmt + ";";
		summary.withheld.push_back (w);
	      }
	  }

	if (hard_error)
	  {
	    break;
	  }
      }

    AU_RESTORE (au_save);

    if (hard_error)
      {
	/* The caller's session_close (false) aborts the whole run. */
	summary = fkdefine_summary ();
	return fkdefine_status::ERR_FKDEFINE;
      }

    /* Say what the engine decided about the data. These two lines used to come
     * from the separate re-validation phase; the information is the same, it is
     * just the FK build that produced it now. */
    if (validate.violated_edges == 0)
      {
	fprintf (stdout, msg (IMPORTDB_MSG_VALIDATE_COMPLETE), graph.database_name.c_str (),
		 validate.validated_edges, (int) rebuild.withheld.size ());
      }
    else
      {
	fprintf (stdout, msg (IMPORTDB_MSG_VALIDATE_VIOLATIONS), graph.database_name.c_str (),
		 validate.violated_edges, (long) validate.total_orphans,
		 path_join (iset.dump_dir, EXCEPTIONS_BASENAME).c_str ());
      }

    /* Record every withheld FK's re-add DDL in the exceptions artifact so an
     * operator has the exact repair statement (appended after any WU-33 orphan
     * records, or creating the file when a Rebuild-phase withhold is the only
     * cause). A write failure is a hard error - abort. */
    if (!summary.withheld.empty ())
      {
	summary.exceptions_file = EXCEPTIONS_BASENAME;
	const std::string path = path_join (iset.dump_dir, EXCEPTIONS_BASENAME);

	/* The orphan records first (this TRUNCATES the artifact), then the
	 * withheld-FK section appended after them. Order matters: the withheld
	 * writer replaces from its own marker to EOF, so writing it first and the
	 * orphans second would throw the repair records away. */
	if (validate.violated_edges > 0)
	  {
	    validate.exceptions_file = EXCEPTIONS_BASENAME;
	    if (!write_fk_exceptions (path, iset, graph, validate, continue_on_error))
	      {
		PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_EXCEPTIONS_WRITE_FAILED), path.c_str (), strerror (errno));
		summary = fkdefine_summary ();
		return fkdefine_status::ERR_FKDEFINE;
	      }
	  }

	if (!write_withheld_exceptions (path, summary))
	  {
	    PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_EXCEPTIONS_WRITE_FAILED), path.c_str (), strerror (errno));
	    summary = fkdefine_summary ();
	    return fkdefine_status::ERR_FKDEFINE;
	  }

	fprintf (stdout, msg (IMPORTDB_MSG_FKDEFINE_WITHHELD), (int) summary.defined.size (),
		 (int) summary.withheld.size (), graph.database_name.c_str (), path.c_str ());
	return fkdefine_status::PARTIAL;
      }

    fprintf (stdout, msg (IMPORTDB_MSG_FKDEFINE_COMPLETE), (int) summary.defined.size (),
	     graph.database_name.c_str ());
    return fkdefine_status::OK;
  }

} // namespace cubimport
