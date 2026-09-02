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
 * import_define.cpp - Stage 2 (Definition phase) of the importdb pipeline
 *
 * Connects to the target DB in CS mode and executes the dump's definition DDL
 * to leave a fully-defined but empty target DB (10-design.md §2). For a DEFAULT
 * dump this is the single <prefix>_schema file (CREATE CLASS + columns + ADD
 * SUPERCLASS + serial + partition + PK/UK/FK); for a SPLIT dump it is the
 * schema-class file followed by the constraint files in the _schema_info apply
 * order (both already resolved by Discovery into iset.schema_apply_order). The
 * <prefix>_trigger and <prefix>_indexes artifacts are DEFERRED to terminal
 * WUs, and no object/data file is touched here.
 *
 * DDL execution reuses the SAME public primitive loaddb is built on -
 * db_make_session_for_one_statement_execution () plus the public
 * db_open_buffer / db_compile_statement / db_execute_statement loop
 * (modeled on the core of loaddb's ldr_exec_query_from_file, minus the
 * old-version client-type compat branches and loaddb-specific logging). No
 * loaddb static is exposed and no loaddb load_args is constructed. As loaddb
 * does for its schema file, authorization is disabled around execution for
 * DBA-group members. On a compile/execute error the offending file + line is
 * reported and the whole definition transaction is aborted (all-or-nothing).
 *
 * The connection is owned by the caller (import_session.hpp): define () assumes
 * the one CS-mode session is already open and shared with the Graph builder's
 * catalog snapshot (WU-21). It commits its DDL on success and aborts on error,
 * but never connects or shuts the session down.
 */

#include "import_define.hpp"
#include "import_progress.hpp"

#include "db.h"
#include "authenticate.h"
#include "error_manager.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"
#include "porting.h"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <strings.h>
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

  /*
   * Pre-11.5 dumps name the catalog VIEWS as `CALL ... ON CLASS <view>` targets.
   * 11.5 left the methods on the underlying classes, so such a statement fails
   * with `Method "<m>" not found` and takes the all-or-nothing definition with
   * it. loaddb rewrites the target in the parse tree (ldr_compat_call_target,
   * load_db.c); no PT_NODE is reachable from the installed surface, so the same
   * three targets are rewritten in the buffer before it is parsed:
   *
   *     db_serial, db_authorization  ->  _db_*
   *     db_user                      ->  _db_user, except find_user and login,
   *                                      which the view still carries
   *
   * Literals and comments are copied through, so only a real ON CLASS target is
   * rewritten. The rewrite adds one character and never a newline, so the line
   * numbers in a later diagnostic still match the dump on disk.
   */

  bool
  is_ident_char (char c)
  {
    return isalnum ((unsigned char) c) != 0 || c == '_' || c == '#';
  }

  /* Case-insensitive, and not run together with a longer identifier. */
  bool
  kw_at (const std::string &s, size_t i, const char *word)
  {
    const size_t n = std::strlen (word);
    if (i + n > s.size ())
      {
	return false;
      }
    for (size_t k = 0; k < n; k++)
      {
	if (tolower ((unsigned char) s[i + k]) != tolower ((unsigned char) word[k]))
	  {
	    return false;
	  }
      }
    return i + n == s.size () || !is_ident_char (s[i + n]);
  }

  size_t
  skip_ws (const std::string &s, size_t i)
  {
    while (i < s.size () && isspace ((unsigned char) s[i]) != 0)
      {
	i++;
      }
    return i;
  }

  /* End of the single-quoted literal at i; '' is an escaped quote, not its end. */
  size_t
  literal_end (const std::string &s, size_t i)
  {
    for (i++; i < s.size (); i++)
      {
	if (s[i] != '\'')
	  {
	    continue;
	  }
	if (i + 1 < s.size () && s[i + 1] == '\'')
	  {
	    i++;
	    continue;
	  }
	return i + 1;
      }
    return s.size ();
  }

  /* End of the comment at i, or i when none starts there. */
  size_t
  comment_end (const std::string &s, size_t i)
  {
    if (i + 1 >= s.size ())
      {
	return i;
      }
    if (s[i] == '-' && s[i + 1] == '-')
      {
	const size_t nl = s.find ('\n', i + 2);
	return (nl == std::string::npos) ? s.size () : nl;
      }
    if (s[i] == '/' && s[i + 1] == '*')
      {
	const size_t close = s.find ("*/", i + 2);
	return (close == std::string::npos) ? s.size () : close + 2;
      }
    return i;
  }

  /* A statement's first token can sit behind a comment block, and the db_user
   * exemption below depends on reaching it. */
  size_t
  skip_trivia (const std::string &s, size_t i)
  {
    for (size_t before = s.size () + 1; i != before;)
      {
	before = i;
	i = comment_end (s, skip_ws (s, i));
      }
    return i;
  }

  /* One identifier at i, bracketed ([name]) or bare. */
  bool
  read_ident (const std::string &s, size_t i, std::string &name, bool &bracketed, size_t &end)
  {
    bracketed = false;
    if (i < s.size () && s[i] == '[')
      {
	const size_t close = s.find (']', i + 1);
	if (close == std::string::npos)
	  {
	    return false;
	  }
	bracketed = true;
	name = s.substr (i + 1, close - i - 1);
	end = close + 1;
	return !name.empty ();
      }
    size_t j = i;
    while (j < s.size () && is_ident_char (s[j]))
      {
	j++;
      }
    if (j == i)
      {
	return false;
      }
    name = s.substr (i, j - i);
    end = j;
    return true;
  }

  bool
  ident_is (const std::string &name, const char *want)
  {
    return name.size () == std::strlen (want) && strcasecmp (name.c_str (), want) == 0;
  }

  /* Method name of the CALL statement at stmt_start, or "" when it is not one. */
  std::string
  call_method_of (const std::string &s, size_t stmt_start, size_t upto)
  {
    size_t i = skip_trivia (s, stmt_start);
    if (!kw_at (s, i, "call"))
      {
	return std::string ();
      }
    i = skip_ws (s, i + 4);
    std::string name;
    bool bracketed = false;
    size_t end = 0;
    if (!read_ident (s, i, name, bracketed, end) || end > upto)
      {
	return std::string ();
      }
    return name;
  }

  /* `unrewritten` collects every ON CLASS target this pass declined to move and
   * that is not already an underlying class, db_root, or the exempt db_user --
   * i.e. a catalog name 11.5 may have renamed without importdb knowing. A
   * definition failure quotes it, because otherwise the failure reads as an
   * arbitrary parse error. Collected in this loop rather than by a second scan:
   * the exemptions are decided here, and a scan that did not know about them
   * reported the exempt target as the suspect. */
  std::string
  rewrite_pre115_call_targets (const std::string &s, int &rewritten, std::vector<std::string> *unrewritten)
  {
    static const char *const RENAMED[] = { "db_serial", "db_authorization", "db_user" };

    std::string out;
    out.reserve (s.size () + 64);
    rewritten = 0;

    size_t stmt_start = 0;
    size_t i = 0;
    while (i < s.size ())
      {
	if (s[i] == '\'')
	  {
	    const size_t end = literal_end (s, i);
	    out.append (s, i, end - i);
	    i = end;
	    continue;
	  }

	const size_t cend = comment_end (s, i);
	if (cend != i)
	  {
	    out.append (s, i, cend - i);
	    i = cend;
	    continue;
	  }

	if (s[i] == ';')
	  {
	    stmt_start = i + 1;
	  }

	if ((s[i] == 'o' || s[i] == 'O') && (i == 0 || !is_ident_char (s[i - 1])) && kw_at (s, i, "on"))
	  {
	    const size_t j = skip_ws (s, i + 2);
	    const size_t k = kw_at (s, j, "class") ? skip_ws (s, j + 5) : i;
	    std::string target;
	    bool bracketed = false;
	    size_t end = 0;
	    if (k != i && read_ident (s, k, target, bracketed, end))
	      {
		bool renamed = false;
		for (const char *r : RENAMED)
		  {
		    renamed = renamed || ident_is (target, r);
		  }
		if (renamed && ident_is (target, "db_user"))
		  {
		    const std::string m = call_method_of (s, stmt_start, i);
		    renamed = !ident_is (m, "find_user") && !ident_is (m, "login");
		  }
		if (!renamed && unrewritten != NULL && target[0] != '_' && !ident_is (target, "db_root")
		    && !ident_is (target, "db_user"))
		  {
		    unrewritten->push_back (target);
		  }
		if (renamed)
		  {
		    out.append (s, i, k - i);	/* "on class" and its spacing, verbatim */
		    out += bracketed ? "[_" : "_";
		    out += target;
		    if (bracketed)
		      {
			out += ']';
		      }
		    i = end;
		    rewritten++;
		    continue;
		  }
	      }
	  }

	out += s[i];
	i++;
      }
    return out;
  }

  /* A dump can carry a name collision of its own making. 11.2 writes synonyms
   * before classes so a view may use one, and an unloaddb before 11.2 Patch 7
   * also wrote every class unqualified and moved the owner afterwards
   * (CBRD-24974). Together, `CREATE PRIVATE SYNONYM [DBA].[t]` followed by
   * `CREATE CLASS [t]` asks for a DBA-owned `t` that the synonym already is:
   *
   *     Class dba.sy_t already exists.
   *
   * Which is what the engine also says when the target simply is not empty --
   * the shape the refusals case asserts -- and the two need opposite responses
   * from the operator. So say which one this is. Measured against an 11.2.6 dump.
   *
   * Called only on a definition failure, so a healthy run does not pay for it. */
  std::string
  colliding_synonym (const std::string &buf)
  {
    std::vector<std::string> synonyms, bare_classes;
    size_t i = 0;

    while (i < buf.size ())
      {
	if (buf[i] == '\'')
	  {
	    i = literal_end (buf, i);
	    continue;
	  }
	const size_t cend = comment_end (buf, i);
	if (cend != i)
	  {
	    i = cend;
	    continue;
	  }
	if ((buf[i] != 'c' && buf[i] != 'C') || (i > 0 && is_ident_char (buf[i - 1])) || !kw_at (buf, i, "create"))
	  {
	    i++;
	    continue;
	  }

	size_t j = skip_ws (buf, i + 6);
	if (kw_at (buf, j, "private"))
	  {
	    j = skip_ws (buf, j + 7);
	  }
	else if (kw_at (buf, j, "public"))
	  {
	    j = skip_ws (buf, j + 6);
	  }

	const bool is_synonym = kw_at (buf, j, "synonym");
	const bool is_class = kw_at (buf, j, "class") || kw_at (buf, j, "table");
	if (!is_synonym && !is_class)
	  {
	    i += 6;
	    continue;
	  }

	std::string name;
	bool bracketed = false;
	size_t end = 0;
	if (!read_ident (buf, skip_ws (buf, j + (is_synonym ? 7 : 5)), name, bracketed, end))
	  {
	    i += 6;
	    continue;
	  }
	/* `[owner].[name]`: an owner-qualified class cannot collide with a
	 * synonym in another schema, and a synonym is named by its second half. */
	if (end < buf.size () && buf[end] == '.')
	  {
	    std::string second;
	    size_t end2 = 0;
	    if (read_ident (buf, end + 1, second, bracketed, end2))
	      {
		end = end2;
		if (is_synonym)
		  {
		    synonyms.push_back (second);
		  }
	      }
	  }
	else if (is_synonym)
	  {
	    synonyms.push_back (name);
	  }
	else
	  {
	    bare_classes.push_back (name);
	  }
	i = end;
      }

    for (const std::string &sn : synonyms)
      {
	for (const std::string &cl : bare_classes)
	  {
	    if (ident_is (cl, sn.c_str ()))
	      {
		return sn;
	      }
	  }
      }
    return std::string ();
  }

  /* Quote the first catalog target the rewrite declined to move. Not the line the
   * parser reports: the buffer is parsed at open, so a parse error's line is the
   * offending one, but an EXECUTE error -- which `Method "x" not found` is --
   * reports wherever the parser finished, past the end of the file when the
   * failure is in the last statements. */
  void
  hint_old_dump_shape (const std::string &buf, const std::vector<std::string> &unrewritten)
  {
    if (!unrewritten.empty ())
      {
	IMPORT_ERR (msg (IMPORTDB_MSG_DEFINE_PRE115_HINT), unrewritten.front ().c_str ());
      }
    const std::string clash = colliding_synonym (buf);
    if (!clash.empty ())
      {
	IMPORT_ERR (msg (IMPORTDB_MSG_DEFINE_SYNONYM_COLLISION), clash.c_str ());
      }
  }

  /*
   * Execute every SQL statement in one DDL file against the connected target.
   * Returns NO_ERROR on success, otherwise the failing statement's error code
   * (a diagnostic naming the file + line has already been emitted). Reuses the
   * public one-statement-at-a-time session primitive; the caller owns the
   * surrounding transaction (no commit/abort here). When commit is false (a
   * --dry-run) the in-file transaction-boundary statements (unloaddb schema
   * files end with an explicit COMMIT WORK) are skipped, so the whole
   * definition stays in one transaction the caller can abort intact.
   */
  int
  exec_ddl_file (const std::string &file_path, bool commit)
  {
    /* Read the file into a buffer and open a BUFFER session, the way csql does for
     * its own file input (csql.c:2218 db_open_buffer). The file-stream primitive
     * db_make_session_for_one_statement_execution () was the obvious reuse of
     * loaddb's shape, but it costs replication: parser->original_buffer is set only
     * on the string-parsing path (parse_tree_cl.c:1983), sql_user_text is copied
     * from it (parse_tree_cl.c:2456), and do_replicate_statement () returns early
     * when sql_user_text is empty -- with the comment "this should be loaddb"
     * (execute_statement.c:16868). Under a file session every CREATE CLASS here is
     * therefore skipped for HA replication while the later ALTER CLASS statements
     * of strip/rebuild/fkdefine, which run through db_execute () on a string, are
     * replicated. The slave then receives ALTER for classes it was never told to
     * create. Measured on a two-node pair: 22 apply failures, 0 rows, no user
     * classes (feature_tests/importdb/m5/ha51b-docker/findings/).
     *
     * The statement semantics are unchanged -- same compile/execute loop, same
     * order, same transaction ownership. What changes is that the whole file is
     * parsed at open rather than one statement at a time, so a syntax error
     * anywhere surfaces here instead of when execution reaches it. Schema files are
     * small (the object data is a different file entirely), so holding one in
     * memory is bounded. */
    std::string buf;
    {
      FILE *fp = fopen (file_path.c_str (), "r");
      if (fp == NULL)
	{
	  IMPORT_ERR (msg (IMPORTDB_MSG_DEFINE_FILE_OPEN_FAILED), file_path.c_str (), strerror (errno));
	  return ER_GENERIC_ERROR;
	}
      char chunk[8192];
      size_t n;
      while ((n = fread (chunk, 1, sizeof (chunk), fp)) > 0)
	{
	  buf.append (chunk, n);
	}
      fclose (fp);
    }

    /* a dump from 11.5 or newer has no such target and comes back unchanged */
    int rewritten = 0;
    std::vector<std::string> unrewritten;
    buf = rewrite_pre115_call_targets (buf, rewritten, &unrewritten);
    if (rewritten > 0)
      {
	IMPORT_PRINT (msg (IMPORTDB_MSG_DEFINE_COMPAT_REWRITE), rewritten, file_path.c_str ());
      }

    DB_SESSION *session = db_open_buffer (buf.c_str ());
    if (session == NULL || db_get_errors (session) != NULL)
      {
	int line = 0, col = 0;
	if (session != NULL)
	  {
	    db_get_parser_line_col (session, &line, &col);
	  }
	IMPORT_ERR (msg (IMPORTDB_MSG_DEFINE_STMT_FAILED), file_path.c_str (), line, db_error_string (3));
	hint_old_dump_shape (buf, unrewritten);
	if (session != NULL)
	  {
	    db_close_session (session);
	  }
	return (er_errid () != NO_ERROR) ? er_errid () : ER_GENERIC_ERROR;
      }

    int error = NO_ERROR;
    int executed = 0;
    while (true)
      {
	/* A buffer session has already parsed every statement; db_compile_statement
	 * walks them in order and returns 0 at the end (csql.c uses the same loop). */
	int stmt_id = db_compile_statement (session);
	int stmt_cnt = stmt_id;

	/* stmt_cnt <= 0 is either clean EOF (no session error) or a parse
	 * error; stmt_id <= 0 is a compile error. Report any and stop. */
	if (stmt_cnt <= 0 || stmt_id <= 0)
	  {
	    DB_SESSION_ERROR *session_error = db_get_errors (session);
	    if (session_error != NULL)
	      {
		int line, col;
		do
		  {
		    session_error = db_get_next_error (session_error, &line, &col);
		    if (line <= 0)
		      {
			db_get_parser_line_col (session, &line, &col);
		      }
		    IMPORT_ERR (msg (IMPORTDB_MSG_DEFINE_STMT_FAILED), file_path.c_str (), line,
					   db_error_string (3));
		    hint_old_dump_shape (buf, unrewritten);
		    assert (er_errid () != NO_ERROR);
		    error = er_errid ();
		  }
		while (session_error);
	      }
	    db_close_session (session);
	    break;
	  }

	/* On a --dry-run the definition must stay in ONE abortable transaction,
	 * but unloaddb schema files end with an explicit COMMIT WORK (and could
	 * carry other transaction-boundary statements). Skip executing those so
	 * nothing is committed mid-define; the caller's session_close(false)
	 * then rolls the entire definition back and the target is untouched. */
	if (!commit)
	  {
	    int stmt_type = db_get_statement_type (session, stmt_id);
	    if (stmt_type == CUBRID_STMT_COMMIT_WORK || stmt_type == CUBRID_STMT_ROLLBACK_WORK)
	      {
		continue;
	      }
	  }

	DB_QUERY_RESULT *res = NULL;
	error = db_execute_statement (session, stmt_id, &res);
	if (error < 0)
	  {
	    int line, col;
	    db_get_parser_line_col (session, &line, &col);
	    IMPORT_ERR (msg (IMPORTDB_MSG_DEFINE_STMT_FAILED), file_path.c_str (), line, db_error_string (3));
	hint_old_dump_shape (buf, unrewritten);
	    db_close_session (session);
	    break;
	  }
	cubimport::progress::set_detail ("executed " + std::to_string (++executed) + " definition statement(s)");
	error = db_query_end (res);
	if (error < 0)
	  {
	    int line, col;
	    db_get_parser_line_col (session, &line, &col);
	    IMPORT_ERR (msg (IMPORTDB_MSG_DEFINE_STMT_FAILED), file_path.c_str (), line, db_error_string (3));
	hint_old_dump_shape (buf, unrewritten);
	    db_close_session (session);
	    break;
	  }
      }

    return error;
  }

  /*
   * Execute the definition DDL for iset in the connected target. DEFAULT runs
   * the single schema file; SPLIT runs the schema-class file then the
   * constraint files, both already in apply order in schema_apply_order. commit
   * is threaded to exec_ddl_file so a --dry-run keeps the whole definition in
   * one abortable transaction.
   */
  int
  run_definition_ddl (const cubimport::import_set &iset, bool commit)
  {
    if (iset.schema_kind == cubimport::schema_layout::SPLIT)
      {
	for (const std::string &f : iset.schema_apply_order)
	  {
	    int error = exec_ddl_file (path_join (iset.dump_dir, f), commit);
	    if (error != NO_ERROR)
	      {
		return error;
	      }
	  }
	return NO_ERROR;
      }

    return exec_ddl_file (path_join (iset.dump_dir, iset.schema_file), commit);
  }
} // namespace

namespace cubimport
{

  define_status
  define (const import_set &iset, bool commit)
  {
    /* This is the definition phase only: trigger actions must not fire. */
    db_disable_trigger ();

    /* As ldr_load_schema_file does, DBA-group members run the definition DDL
     * with authorization disabled (compat with pre-authorization dumps). */
    int au_save = 0;
    AU_SAVE_AND_DISABLE (au_save);
    int error = run_definition_ddl (iset, commit);
    AU_RESTORE (au_save);

    if (error != NO_ERROR)
      {
	db_abort_transaction ();
	return define_status::ERR_DDL;
      }

    /* On a normal run commit so the target is durably defined and empty. On a
     * --dry-run (commit=false) leave the DDL uncommitted in the open
     * transaction: the Graph builder's snapshot still sees it on this session,
     * and the caller's session_close(false) aborts it so the target is left
     * completely unchanged (CUBRID DDL is transactional). */
    if (commit)
      {
	db_commit_transaction ();
      }
    IMPORT_PRINT (msg (IMPORTDB_MSG_DEFINE_COMPLETE), iset.database_name.c_str ());
    return define_status::OK;
  }

} // namespace cubimport
