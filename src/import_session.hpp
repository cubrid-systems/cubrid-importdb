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
 * import_session.hpp - The importdb server session (one connection per run)
 *
 * The §4 lifecycle (define -> snapshot -> strip -> load -> rebuild) runs on a
 * single open connection to the target DB: every stage that touches the server
 * (define, then the catalog snapshot the Graph builder reads, and later strip/
 * load/rebuild) must see the SAME session so the catalog stays consistent. This
 * helper opens that connection ONCE in CS mode as the admin utility (honoring
 * importdb's -u/-p, DBA-group-gated) and closes it ONCE at the end (commit or
 * abort + shutdown). The pipeline stages themselves assume the session is open.
 */

#ifndef _IMPORT_SESSION_HPP_
#define _IMPORT_SESSION_HPP_

namespace cubimport
{

  /*
   * Session-open outcome. OK means the target DB is connected and the import
   * user is a DBA-group member; every other value corresponds to a named
   * diagnostic already emitted by session_open ().
   */
  enum class session_status
  {
    OK = 0,
    ERR_CONNECT,		/* could not connect/restart the target DB */
    ERR_NOT_DBA			/* the import user is not in the DBA group */
  };

  /*
   * Open the one importdb session: connect to database_name in CS mode as a
   * general admin utility (mirrors loaddb_internal and the CS admin utilities),
   * honoring -u/-p (a NULL/empty user_name defaults to DBA), and require the
   * user to belong to the DBA group. On failure emits the matching named
   * diagnostic (and shuts the connection back down) and returns a non-OK status.
   */
  session_status session_open (const char *command_name, const char *database_name, const char *user_name,
			       const char *password);

  /*
   * Commit the current transaction WITHOUT closing the connection. Used before
   * the parallel data phase (WU-40): the inter-table load spawns independent
   * `loaddb -C` client processes, each its own connection/transaction, so the
   * strip must be durable (bare heaps committed) for those children to see it -
   * unlike the serial in-process load, which reads the caller's uncommitted
   * strip on the shared session. A no-op if nothing is pending.
   *
   * Returns true when the commit succeeded. **The caller must check it.** Since
   * WU-50 this is the load-bearing call of the whole pipeline: every phase
   * commits here and only then advances the manifest's phase marker, so
   * "reached X" means X's work is durable. A commit that fails while the marker
   * advances anyway puts the marker AHEAD of the database - the one state no
   * resume guard can absorb, because every guard is keyed off a catalog the
   * manifest would then be lying about. On false the caller must emit
   * IMPORTDB_MSG_COMMIT_FAILED, NOT write the manifest, and fail the run.
   */
  bool session_commit ();

  /*
   * Close the importdb session: commit (commit=true) or abort (commit=false)
   * the current transaction, then shut the connection down. Called exactly once
   * at the end of the run, on every exit path after session_open () returned OK.
   * Returns true unless a requested commit failed - in which case the work of
   * the final phase was lost and the run must exit non-zero however clean its
   * per-phase records look.
   */
  bool session_close (bool commit);

} // namespace cubimport

#endif /* _IMPORT_SESSION_HPP_ */
