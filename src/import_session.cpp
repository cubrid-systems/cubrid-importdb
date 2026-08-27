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
 * import_session.cpp - The importdb server session (one connection per run)
 *
 * Opens the single CS-mode connection the whole pipeline shares and tears it
 * down once at the end. Connecting mirrors loaddb_internal and the other CS
 * admin utilities: set the ADMIN_UTILITY client type, db_login the -u/-p user
 * (default DBA), db_restart the target, then require DBA-group membership.
 * Previously the Definition phase (WU-20) connected and shut down internally;
 * that connect/shutdown moved here so define () and the Graph builder's catalog
 * snapshot operate on the same open session (10-design.md §4 lifecycle).
 */

#include "import_session.hpp"

#include "db.h"
#include "db_client_type.hpp"
#include "authenticate.h"

#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"

#include <cstdlib>

namespace
{
  const char *
  msg (int id)
  {
    return msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, id);
  }
} // namespace

namespace cubimport
{

  session_status
  session_open (const char *command_name, const char *database_name, const char *user_name, const char *password)
  {
    const char *user = (user_name != NULL && user_name[0] != '\0') ? user_name : "DBA";

    /* Connect in CS mode as the general admin utility (mirrors loaddb_internal
     * and the CS admin utilities), honoring -u/-p; the default user is DBA. */
    db_set_client_type (DB_CLIENT_TYPE_ADMIN_UTILITY);
    (void) db_login (user, password);
    if (db_restart (command_name, true, database_name) != NO_ERROR)
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_CONNECT_FAILED), database_name, db_error_string (3));
	return session_status::ERR_CONNECT;
      }

    /* The import user must belong to the DBA group (the whole pipeline needs
     * it: DDL define + the catalog snapshot, incl. underlying system classes). */
    if (!au_is_dba_group_member (Au_user))
      {
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_NOT_DBA), user);
	db_shutdown ();
	return session_status::ERR_NOT_DBA;
      }

    return session_status::OK;
  }

  bool
  session_commit ()
  {
    /* Test hook. The "commit, then advance the marker" rule is what the whole
     * resume model rests on, and its FAILURE path is otherwise unreachable from
     * a test: a commit does not fail on demand. IMPORTDB_TEST_FAIL_COMMIT_AT=<n>
     * makes the n-th commit of the run fail the way the server would - the
     * transaction is aborted, so the phase's work really is gone - which lets
     * m5/wu50_resume_test.sh assert that the manifest does NOT advance past it
     * and that a re-run still converges on the correct database. Unset, this
     * costs one getenv per phase and does nothing. */
    static int commits = 0;
    commits++;
    const char *const fail_at = getenv ("IMPORTDB_TEST_FAIL_COMMIT_AT");
    if (fail_at != NULL && atoi (fail_at) == commits)
      {
	db_abort_transaction ();
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_COMMIT_FAILED),
			       "injected by IMPORTDB_TEST_FAIL_COMMIT_AT (test hook)");
	return false;
      }

    if (db_commit_transaction () != NO_ERROR)
      {
	/* The caller decides what to do; it must not advance the manifest. */
	PRINT_AND_LOG_ERR_MSG (msg (IMPORTDB_MSG_COMMIT_FAILED), db_error_string (3));
	return false;
      }
    return true;
  }

  bool
  session_close (bool commit)
  {
    /* Through the same checked helper, so the final commit gets the same
     * diagnostic - and the same test hook - as every phase boundary. By the time
     * this runs the pipeline has already committed each phase, so a normal close
     * has nothing left to write; a failure here still has to reach the exit
     * code rather than be swallowed behind clean-looking per-phase records. */
    const bool ok = commit ? session_commit () : (db_abort_transaction (), true);

    db_shutdown ();
    return ok;
  }

} // namespace cubimport
