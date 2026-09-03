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
 * import_rebuild.cpp - Stage 4 (Rebuild phase, step 3 of the constraint lifecycle)
 *
 * Re-adds the stripped PK/UNIQUE constraints and builds the deferred plain
 * indexes on the loaded target (10-design.md §6). It re-executes the dump's OWN
 * PK/UK ADD statements verbatim - for SPLIT the isolated <prefix>_schema_pk then
 * <prefix>_schema_uk files, for DEFAULT the ALTER CLASS ... ADD ATTRIBUTE
 * CONSTRAINT [<name>] PRIMARY KEY|UNIQUE(...) statements isolated from the single
 * <prefix>_schema by matching the stripped PK/UK names - then the plain
 * <prefix>_indexes CREATE INDEXes. Statements run one at a time over the same
 * public single-statement primitive the Strip phase uses (db_execute) on the
 * already-open session, authorization disabled for the DBA-group path; on a
 * populated table an ADD CONSTRAINT bulk-builds the b-tree server-side. loaddb is
 * not touched.
 *
 * The §6 rebuild-failure contract is enforced per constraint: a PK/UNIQUE ADD
 * that fails (duplicate key) is left un-rebuilt and recorded in summary.pending,
 * every FK edge whose PARENT is that class is recorded in summary.withheld (WU-34
 * honors it - no FK is defined yet here), and the phase CONTINUES to the other
 * rebuilds. When any constraint (or index) failed, rebuild () returns PARTIAL so
 * the caller commits the successfully-rebuilt constraints + the loaded data yet
 * exits non-zero. The caller owns the transaction (import_session.hpp): rebuild ()
 * neither commits nor aborts.
 */

#include "import_rebuild.hpp"
#include "import_progress.hpp"
#include "import_resume.hpp"

#include "db.h"
#include "authenticate.h"
#include "error_manager.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <cctype>
#include <cerrno>
#include <cstring>
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

  std::string
  path_join (const std::string &dir, const std::string &name)
  {
    if (dir.empty () || dir.back () == '/')
      {
	return dir + name;
      }
    return dir + "/" + name;
  }

  /* The DDL keyword for a stripped kind, used in the per-constraint failure
   * diagnostic (only PK/UNIQUE reach the rebuild). */
  const char *
  rebuild_kind_keyword (cubimport::stripped_kind kind)
  {
    switch (kind)
      {
      case cubimport::stripped_kind::PK:
	return "PRIMARY KEY";
      case cubimport::stripped_kind::UNIQUE:
	return "UNIQUE";
      case cubimport::stripped_kind::FK:
	return "FOREIGN KEY";
      }
    return "";
  }

  /* True when name ends with suffix (used to pick the split PK/UK files out of
   * the schema apply order). */
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

  /* Split a DDL file's text into statements on the semicolon terminator (PK/UK
   * ADD and CREATE INDEX statements carry no embedded semicolons or strings),
   * trimming each and dropping empties. */
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

  /* The class an ALTER statement targets: the last bracketed identifier before its
   * ADD keyword, so unloaddb's owner-qualified "ALTER CLASS [dba].[ta] ADD" and a
   * bare "ALTER CLASS [ta] ADD" both give the name the catalog uses. Empty when
   * the shape does not match, which matches no stripped constraint and so leaves
   * the reconciliation at the end of rebuild () to report it rather than dropping
   * the constraint in silence. */
  std::string
  statement_class (const std::string &stmt)
  {
    const size_t add = to_upper (stmt).find (" ADD ");
    if (add == std::string::npos)
      {
	return std::string ();
      }
    const size_t rb = stmt.rfind (']', add);
    const size_t lb = rb == std::string::npos ? std::string::npos : stmt.rfind ('[', rb);
    return lb == std::string::npos ? std::string () : stmt.substr (lb + 1, rb - lb - 1);
  }

  /* A constraint is identified by class AND name: CUBRID index names are unique
   * per class, not per database, so two classes may each carry a [u1]. */
  std::string
  constraint_key (const std::string &cls, const std::string &name)
  {
    return cls + "\t" + name;
  }

  /* Extract every constraint name from a statement's "CONSTRAINT [<name>]"
   * clauses (case-insensitive on the keyword; the bracketed name keeps its
   * original case). A DEFAULT-layout class may add its PK and a standalone UNIQUE
   * in one ALTER CLASS statement, so this can return more than one. */
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

  /* The index name from a "CREATE [UNIQUE] INDEX [<name>] ON ..." statement (the
   * first bracketed token after INDEX); empty when it cannot be found. */
  std::string
  index_name (const std::string &stmt)
  {
    const std::string upper = to_upper (stmt);
    size_t kw = upper.find ("INDEX");
    if (kw == std::string::npos)
      {
	return "";
      }
    size_t lb = stmt.find ('[', kw);
    if (lb == std::string::npos)
      {
	return "";
      }
    size_t rb = stmt.find (']', lb + 1);
    if (rb == std::string::npos)
      {
	return "";
      }
    return stmt.substr (lb + 1, rb - lb - 1);
  }

  /* The class name from a "CREATE [UNIQUE] INDEX [<name>] ON [<owner>].[<class>]
   * (...)" statement; empty when it cannot be found. Needed by the WU-50 resume
   * guard, since an index name is unique only within its class - and the name
   * the catalog holds is the bare class, so the OWNER qualifier unloaddb writes
   * has to be stepped over: take the last bracketed token between " ON " and the
   * column list. */
  std::string
  index_class (const std::string &stmt)
  {
    const std::string upper = to_upper (stmt);
    const size_t kw = upper.find (" ON ");
    if (kw == std::string::npos)
      {
	return "";
      }
    const size_t cols = stmt.find ('(', kw);
    const size_t end = (cols == std::string::npos) ? stmt.size () : cols;

    std::string last;
    for (size_t lb = stmt.find ('[', kw); lb != std::string::npos && lb < end; lb = stmt.find ('[', lb + 1))
      {
	const size_t rb = stmt.find (']', lb + 1);
	if (rb == std::string::npos || rb > end)
	  {
	    break;
	  }
	last = stmt.substr (lb + 1, rb - lb - 1);
      }
    return last;
  }

  /*
   * Compile + execute one statement on the open session (the same public
   * single-statement primitive the Strip phase uses). Returns NO_ERROR, or the
   * negative db_execute error code - the caller captures db_error_string () and
   * decides whether to record + continue (PK/UK) or report a hard error. A CUBRID
   * statement failure does not abort the transaction, so the next statement can
   * still run (the §6 continue-on-failure contract relies on this).
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

  /*
   * Cascade a class's failed PK/UNIQUE rebuild onto the FK edges it parents:
   * every FK edge whose parent is cls must be withheld from WU-34's FK define.
   * Records each such edge once (deduplicated by FK name) in summary.withheld.
   */
  void
  withhold_child_fks (const cubimport::dependency_graph &graph, const std::string &cls, const std::string &key_name,
		      std::set<std::string> &already, cubimport::rebuild_summary &summary)
  {
    for (const cubimport::fk_edge &e : graph.fk_edges)
      {
	if (e.parent == cls && already.insert (e.name).second)
	  {
	    cubimport::withheld_fk w;
	    w.child = e.child;
	    w.parent = e.parent;
	    w.name = e.name;
	    w.reason = "parent '" + cls + "' key [" + key_name + "] failed rebuild";
	    summary.withheld.push_back (w);
	  }
      }
  }
} // namespace

namespace cubimport
{

  rebuild_status
  rebuild (const import_set &iset, const dependency_graph &graph, const std::vector<stripped_constraint> &stripped,
	   rebuild_summary &summary, const catalog_state *present)
  {
    summary = rebuild_summary ();

    /* The stripped PK/UNIQUE constraints keyed by class and name -- not by name
     * alone, which resolved every statement for a repeated name to whichever class
     * was recorded last. This is both the DEFAULT layout's isolation filter (a
     * <prefix>_schema statement is a rebuild target only when its class and
     * constraint name are here) and the authority for each rebuilt/pending
     * record's class + kind. FK names are excluded, so FK ADD statements are never
     * picked up. */
    std::map<std::string, const stripped_constraint *> pkuk_by_key;
    for (const stripped_constraint &c : stripped)
      {
	if (c.kind == stripped_kind::PK || c.kind == stripped_kind::UNIQUE)
	  {
	    pkuk_by_key[constraint_key (c.cls, c.name)] = &c;
	  }
      }

    /* The PK/UK statement sources, in rebuild order (PK before UNIQUE). SPLIT
     * runs the isolated <prefix>_schema_pk then <prefix>_schema_uk (either may be
     * absent); DEFAULT runs the single interleaved <prefix>_schema, from which the
     * name filter isolates just the PK/UK statements. */
    std::vector<std::string> pkuk_files;
    if (iset.schema_kind == schema_layout::SPLIT)
      {
	for (const std::string &f : iset.schema_apply_order)
	  {
	    if (ends_with (f, "_schema_pk"))
	      {
		pkuk_files.push_back (f);
	      }
	  }
	for (const std::string &f : iset.schema_apply_order)
	  {
	    if (ends_with (f, "_schema_uk"))
	      {
		pkuk_files.push_back (f);
	      }
	  }
      }
    else
      {
	pkuk_files.push_back (iset.schema_file);
      }

    /* As the Strip phase does, DBA-group members run the rebuild DDL with
     * authorization disabled. */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);

    bool partial = false;
    bool hard_error = false;
    std::set<std::string> withheld_names;

    for (const std::string &f : pkuk_files)
      {
	std::string text;
	const std::string full = path_join (iset.dump_dir, f);
	if (read_file_text (full, text) != NO_ERROR)
	  {
	    IMPORT_ERR (msg (IMPORTDB_MSG_REBUILD_FILE_OPEN_FAILED), full.c_str (), strerror (errno));
	    hard_error = true;
	    break;
	  }

	for (const std::string &stmt : split_statements (text))
	  {
	    /* Match this statement's constraint clauses against the stripped
	     * PK/UK set; a statement with no matched name is not a PK/UK rebuild
	     * (CREATE CLASS / column / serial / FK / COMMIT WORK) and is skipped. */
	    std::vector<const stripped_constraint *> targets;
	    const std::string stmt_cls = statement_class (stmt);
	    for (const std::string &nm : constraint_names (stmt))
	      {
		std::map<std::string, const stripped_constraint *>::iterator it =
			pkuk_by_key.find (constraint_key (stmt_cls, nm));
		if (it != pkuk_by_key.end ())
		  {
		    targets.push_back (it->second);
		  }
	      }
	    if (targets.empty ())
	      {
		continue;
	      }
	    cubimport::progress::set_counter ((int) (summary.rebuilt.size () + summary.pending.size ()),
					      (int) pkuk_by_key.size (), targets.front ()->name);

	    /* WU-50 resume guard: an interrupted prior rebuild may already have
	     * re-added this constraint. Re-adding it would fail on "already
	     * exists" and be recorded as a pending rebuild, so record it as
	     * rebuilt - which it is - and move on. A statement carrying several
	     * constraints is only skipped when every one of them is back. */
	    if (present != NULL)
	      {
		bool all_present = true;
		for (const stripped_constraint *c : targets)
		  {
		    if (!present->has_index (c->cls, c->name))
		      {
			all_present = false;
			break;
		      }
		  }
		if (all_present)
		  {
		    for (const stripped_constraint *c : targets)
		      {
			summary.rebuilt.push_back ({ c->kind, c->cls, c->name });
		      }
		    continue;
		  }
	      }

	    int error = exec_stmt (stmt + ";");
	    if (error == NO_ERROR)
	      {
		for (const stripped_constraint *c : targets)
		  {
		    summary.rebuilt.push_back ({ c->kind, c->cls, c->name });
		  }
	      }
	    else
	      {
		/* §6 fail-fast-single per constraint: leave un-rebuilt, record the
		 * pending set + the FK cascade, and CONTINUE to the next rebuild. */
		const std::string reason = db_error_string (3);
		for (const stripped_constraint *c : targets)
		  {
		    summary.pending.push_back ({ c->kind, c->cls, c->name, reason, stmt + ";" });
		    IMPORT_ERR (msg (IMPORTDB_MSG_REBUILD_CONSTRAINT_FAILED), rebuild_kind_keyword (c->kind),
					   c->name.c_str (), c->cls.c_str (), reason.c_str ());
		    withhold_child_fks (graph, c->cls, c->name, withheld_names, summary);
		  }
		partial = true;
	      }
	  }
      }

    /* Deferred plain indexes: run the <prefix>_indexes CREATE INDEXes verbatim on
     * the populated tables (skipped after a hard file error). Plain indexes are
     * non-unique, so they do not hit the duplicate-key path; a failure is still
     * recorded and continues (PARTIAL). */
    if (!hard_error && !iset.index_file.empty ())
      {
	std::string text;
	const std::string full = path_join (iset.dump_dir, iset.index_file);
	if (read_file_text (full, text) != NO_ERROR)
	  {
	    IMPORT_ERR (msg (IMPORTDB_MSG_REBUILD_FILE_OPEN_FAILED), full.c_str (), strerror (errno));
	    hard_error = true;
	  }
	else
	  {
	    for (const std::string &stmt : split_statements (text))
	      {
		if (to_upper (stmt).compare (0, 6, "CREATE") != 0)
		  {
		    continue;		/* COMMIT WORK and the like */
		  }
		/* WU-50 resume guard, as above: an index the catalog already
		 * holds was built by the interrupted run. */
		if (present != NULL && present->has_index (index_class (stmt), index_name (stmt)))
		  {
		    summary.indexes.push_back (index_name (stmt));
		    continue;
		  }
		cubimport::progress::set_detail ("building index " + index_name (stmt));
		int error = exec_stmt (stmt + ";");
		if (error == NO_ERROR)
		  {
		    summary.indexes.push_back (index_name (stmt));
		  }
		else
		  {
		    const std::string reason = db_error_string (3);
		    IMPORT_ERR (msg (IMPORTDB_MSG_REBUILD_INDEX_FAILED), index_name (stmt).c_str (),
					   reason.c_str ());
		    /* Recorded, not just counted: the phase is PARTIAL because of
		     * this, and a resumed run re-derives that status from the
		     * summary the manifest carries. */
		    summary.failed_indexes.push_back ({ index_name (stmt), reason });
		    partial = true;
		  }
	      }
	  }
      }

    /* The loop above is driven by the dump's statements, so a stripped constraint
     * that no statement matched is visited by nothing: without this it would be
     * absent from the catalog AND from both records, and the run would report
     * COMPLETE having silently dropped it. */
    if (!hard_error)
      {
	std::set<std::string> accounted;
	for (const rebuilt_constraint &c : summary.rebuilt)
	  {
	    accounted.insert (constraint_key (c.cls, c.name));
	  }
	for (const pending_rebuild &c : summary.pending)
	  {
	    accounted.insert (constraint_key (c.cls, c.name));
	  }
	for (const std::pair<const std::string, const stripped_constraint *> &e : pkuk_by_key)
	  {
	    if (accounted.count (e.first))
	      {
		continue;
	      }
	    const stripped_constraint *c = e.second;
	    const std::string reason = "no ADD CONSTRAINT statement in the dump matched this class and name";
	    summary.pending.push_back ({ c->kind, c->cls, c->name, reason, std::string () });
	    IMPORT_ERR (msg (IMPORTDB_MSG_REBUILD_CONSTRAINT_FAILED), rebuild_kind_keyword (c->kind),
				   c->name.c_str (), c->cls.c_str (), reason.c_str ());
	    withhold_child_fks (graph, c->cls, c->name, withheld_names, summary);
	    partial = true;
	  }
      }

    AU_RESTORE (au_save);

    if (hard_error)
      {
	/* The caller's session_close (false) aborts the whole run. */
	summary = rebuild_summary ();
	return rebuild_status::ERR_REBUILD;
      }

    if (partial)
      {
	IMPORT_PRINT (msg (IMPORTDB_MSG_REBUILD_PARTIAL), (int) summary.rebuilt.size (),
		 (int) summary.pending.size (), graph.database_name.c_str (), (int) summary.withheld.size ());
	return rebuild_status::PARTIAL;
      }

    IMPORT_PRINT (msg (IMPORTDB_MSG_REBUILD_COMPLETE), (int) summary.rebuilt.size (), (int) summary.indexes.size (),
	     graph.database_name.c_str ());
    return rebuild_status::OK;
  }

} // namespace cubimport
