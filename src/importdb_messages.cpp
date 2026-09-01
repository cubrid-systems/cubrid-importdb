/*
 * importdb_messages.cpp -- the utility's own message table.
 *
 * In-tree these strings live in the engine's msg/<locale>/utils.msg as message
 * set 61, compiled into $CUBRID/msg/<locale>/utils.cat. A separate repo cannot
 * put them there, so it carries them itself and the build renames the lookup:
 *
 *     -Dmsgcat_message=importdb_msgcat_message
 *
 * The define is applied before any header, so it renames the declaration in
 * message_catalog.h and every call site together -- which is why the 16 importdb
 * sources need no edits at all.
 *
 * Generated from msg/en_US.utf8/utils.msg $set 61.
 */
#include <cstddef>

extern "C" const char *importdb_msgcat_message (int cat_id, int set_id, int msg_id);

namespace
{
  struct entry { int id; const char *text; };

  const entry importdb_messages[] = {
    { 10, "importdb: '--exceptions-table' is reserved for a future release.\n" },
    { 11, "importdb: '--degree=%1$d' is not yet supported; clamping to serial (degree 1).\n" },
    { 12, "importdb: rostered %1$s dump (prefix '%2$s') from %3$s\n    schema:   %4$s\n    objects:  %5$s\n    indexes:  %6$s\n    triggers: %7$s\n" },
    { 13, "importdb: no unloaddb dump found in '%1$s' (expected <prefix>_schema or <prefix>_schema_class plus <prefix>_objects).\n" },
    { 14, "importdb: dump in '%1$s' has no schema artifact; expected %2$s_schema (default) or %2$s_schema_class with %2$s_schema_info (split).\n" },
    { 15, "importdb: schema manifest %1$s names '%2$s', but that file is missing from the dump.\n" },
    { 16, "importdb: file '%1$s' does not match the dump prefix '%2$s'; the directory mixes more than one dump.\n" },
    { 17, "importdb: dump in '%1$s' (prefix '%2$s') has no object roster (%2$s_objects or %2$s_<owner>.<class>_objects).\n" },
    { 18, "importdb: cannot open dump directory '%1$s'.\n" },
    { 19, "importdb: cannot write manifest '%1$s': %2$s.\n" },
    { 20, "importdb: cannot connect to database '%1$s': %2$s\n" },
    { 21, "importdb: user '%1$s' is not a member of the DBA group.\n" },
    { 22, "importdb: definition failed in %1$s at line %2$d: %3$s\n" },
    { 23, "importdb: cannot open schema file '%1$s': %2$s.\n" },
    { 24, "importdb: defined '%1$s'; the target database is now fully defined and empty.\n" },
    { 25, "importdb: dependency graph for '%1$s' -- %2$d node(s), %3$d FK edge(s), %4$d inheritance edge(s), %5$d serial(s)\n" },
    { 26, "importdb: cannot import object-valued class(es); CS-mode loads value data only: %1$s. Re-run with --skip-object-classes to skip them.\n" },
    { 27, "importdb: skipping %1$d object-valued class(es): %2$s\n" },
    { 28, "importdb: cannot build the dependency graph from the catalog of '%1$s': %2$s\n" },
    { 29, "importdb: schedule for '%1$s' -- data phase %2$d level(s), %3$d terminal task(s)\n" },
    { 30, "importdb: schedule for '%1$s' failed its ordering machine-check: %2$s\n" },
    { 31, "importdb: DRY RUN -- printed the import plan for '%1$s' only; no changes were made and the target database is untouched.\n" },
    { 32, "importdb: strip failed on statement '%1$s': %2$s\n" },
    { 33, "importdb: stripped %1$d constraint(s) from '%2$s'; the target now holds bare heaps ready for the data phase.\n" },
    { 34, "importdb: loading object data from '%1$s' failed: %2$s\n" },
    { 35, "importdb: loaded %1$ld row(s) from %2$d object file(s) into '%3$s'; the target now holds data in bare heaps (constraints are rebuilt in a later phase).\n" },
    { 36, "importdb: rebuild of %1$s constraint [%2$s] on class '%3$s' failed: %4$s\n" },
    { 37, "importdb: building deferred index [%1$s] failed: %2$s\n" },
    { 38, "importdb: cannot open rebuild source '%1$s': %2$s.\n" },
    { 39, "importdb: rebuilt %1$d constraint(s) and built %2$d index(es) on '%3$s'; PK/UNIQUE and plain indexes are restored (FK definition is a later phase).\n" },
    { 40, "importdb: rebuilt %1$d constraint(s), %2$d failed on '%3$s'; %4$d dependent FK(s) withheld from the FK phase. See the manifest [rebuild] section; importdb exits non-zero.\n" },
    { 41, "importdb: the engine rejected the FOREIGN KEY and found %1$d orphan row(s) on '%2$s' -> '%3$s' [%4$s]; that FK is withheld and its offenders enumerated.\n" },
    { 42, "importdb: every FOREIGN KEY on '%1$s' was accepted by the engine -- %2$d edge(s) clean, %3$d skipped (parent key withheld). The engine validates the rows while it builds each FK.\n" },
    { 43, "importdb: '%1$s' has referential violations -- %2$d FOREIGN KEY(s) rejected by the engine, %3$ld orphan row(s) total; offenders written to %4$s. See the manifest [validate] section; importdb exits non-zero.\n" },
    { 44, "importdb: enumerating the orphan rows of '%1$s' -> '%2$s' [%3$s] failed: %4$s\n" },
    { 45, "importdb: cannot write exceptions artifact '%1$s': %2$s.\n" },
    { 46, "importdb: defined %1$d FK(s) on '%2$s'; the catalog now matches the dump snapshot (full round-trip complete).\n" },
    { 47, "importdb: defined %1$d FK(s), withheld %2$d on '%3$s' (the engine rejected the data, or the parent key was un-rebuilt); re-add DDL recorded in the manifest [fkdefine] section and %4$s. importdb exits non-zero.\n" },
    { 48, "importdb: defining FK [%1$s] on '%2$s' -> '%3$s' failed for a reason that is not the data: %4$s\n" },
    { 49, "importdb: cannot open FK define source '%1$s': %2$s.\n" },
    { 50, "importdb: updated statistics on %1$d class(es) in '%2$s'.\n" },
    { 51, "importdb: updated statistics on %1$d class(es), %2$d failed in '%3$s'. See the manifest [stats] section; importdb exits non-zero.\n" },
    { 52, "importdb: updating statistics on class '%1$s' failed: %2$s\n" },
    { 53, "importdb: defined %1$d trigger(s) from '%2$s'.\n" },
    { 54, "importdb: defined %1$d trigger(s), %2$d failed from '%3$s'. See the manifest [triggers] section; importdb exits non-zero.\n" },
    { 55, "importdb: defining triggers from '%1$s' at line %2$d failed: %3$s\n" },
    { 56, "importdb: cannot open trigger file '%1$s': %2$s.\n" },
    { 57, "importdb: ===== import report: '%1$s' -- %2$s =====\n" },
    { 58, "importdb: '%1$s' import %2$s -- %3$d class(es) done, %4$d skipped, %5$d pending, %6$d FK(s) withheld; %7$ld row(s) loaded. Repair records + re-add DDL in %8$s/importdb.manifest.\n" },
    { 59, "importdb: loaded %1$ld row(s) from %2$d object file(s) into '%3$s' at degree %4$d (inter-table parallel; constraints rebuilt in a later phase).\n" },
    { 60, "importdb: Import an unloaddb dump into a running database.\nusage: %1$s [OPTION] database-name dump-dir\n\nvalid options:\n    -u, --user=ID               import user; must belong to the DBA group\n    -p, --password=PASS         password of the import user\n    --degree=N                  inter-table parallel degree\n    --continue                  attempt every FK edge instead of stopping at the first rejected one\n    --skip-object-classes       skip and report object-valued classes instead of rejecting them\n    --dry-run                   print the import plan and terminal tasks; change nothing\n    --restart                   ignore an interrupted run's manifest and import from scratch\n    --allow-ha                  import into an ha_mode=on target anyway; the standby will NOT\n                                receive the rows and must be rebuilt from a backup\n    --progress=WHEN             live progress display: auto (default; on when stdout is a\n                                terminal), always, or never\n    --exceptions-table=NAME     reserved for a future release\n" },
    { 61, "importdb: cannot skip object-valued class(es) in a single-file object dump; their instances share %1$s_objects and carry object references CS-mode load cannot parse. Re-dump with 'unloaddb --datafile-per-class' to skip them per file. classes: %2$s\n" },
    { 62, "importdb: '%1$s' is already fully imported from this dump -- the manifest reports every phase complete; nothing to do (remove %2$s to force a fresh import).\n" },
    { 63, "importdb: resuming the interrupted import of '%1$s' -- manifest %2$s records phase '%3$s' complete, so the run restarts at the next phase and skips the completed ones.\n" },
    { 64, "importdb: manifest %1$s cannot be resumed (%2$s); importing '%3$s' from scratch.\n" },
    { 65, "importdb: --restart: ignoring the prior manifest %1$s and importing '%2$s' from scratch.\n" },
    { 66, "importdb: emptying class '%1$s' before the resumed data phase failed: %2$s\n" },
    { 67, "importdb: emptied %1$d class(es); the resumed data phase reloads them in full and the interrupted run's partial rows are discarded.\n" },
    { 68, "importdb: cannot read the catalog of '%1$s' for the resume guard: %2$s\n" },
    { 69, "importdb: '%1$s' was already imported from this dump, but that import did not complete cleanly -- %2$d constraint(s) left un-rebuilt, %3$d FK(s) withheld, %4$d class(es) without refreshed statistics, %5$d trigger statement(s) failed. Nothing was re-run: the repair records and their re-add DDL are in %6$s, and importdb keeps exiting non-zero until they are resolved (repair the data and apply them, or --restart into an empty target).\n" },
    { 70, "importdb: commit failed: %1$s. The work of the phase that just finished was rolled back and the manifest was NOT advanced, so it still records the last durable phase -- re-run the same command to continue from there.\n" },
    { 71, "importdb: refusing to resume into '%1$s': %2$s. The manifest describes a different target than the database importdb is connected to, and a resumed run skips the definition phase -- the one step that would otherwise refuse a non-empty database. Point at the right database, or use --restart against an empty one.\n" },
    { 72, "importdb: the previous manifest was preserved as %1$s before this run replaced it -- it is the only record of the interrupted import's progress. To resume that import instead, move it back over %2$s and re-run the command it was started with.\n" },
    { 73, "importdb: manifest '%1$s' exists but cannot be read: %2$s. Refusing to import: a fresh run would define over a target that may already be half-imported. Fix the permissions, or remove the file deliberately.\n" },
    { 74, "importdb: manifest '%1$s' is present but records no phase -- it is truncated or corrupt, so importdb cannot tell how far the interrupted run got. Refusing to act, and the file was left untouched. Inspect it, or use --restart against an empty target.\n" },
    { 75, "importdb: cannot move the existing manifest '%1$s' aside: %2$s. Refusing to continue rather than overwrite it -- it is the only record of the interrupted import's progress.\n" },
    { 76, "importdb: '%1$s' is now PARTIALLY IMPORTED -- everything up to and including the '%2$s' phase is committed and durable, and %3$s records it. Re-run the same command to continue from there; use --restart against an empty target to start over.\n" },
    { 77, "importdb: refusing to import into '%1$s': the target server is an HA %2$s. importdb loads into bare heaps -- it drops PK/UNIQUE/FK before the data phase and rebuilds them after -- and CUBRID writes a row replication record only while walking a primary-key index. With no PK in place during the load, no row is replicated: the standby would end up with a complete, correct, EMPTY copy of this schema, and nothing would report an error. Import against a non-HA target, or pass --allow-ha and rebuild the standby from a backup afterwards.\n" },
    { 80, "importdb: '--progress=%1$s' is not a valid value; use auto, always or never.\n" },
    { 78, "importdb: '%1$s' is an HA %2$s and --allow-ha was given. The data phase will NOT replicate; the standby will not receive these rows.\n" },
    { 79, "importdb: the standby of '%1$s' did NOT receive this import. importdb loaded into bare heaps, so no row replication record was written; the schema replicated but the rows did not. Rebuild the standby from a backup of this database before failing over to it.\n" },
    { 82, "importdb: '%1$s' has data_buffer_size=%2$s for %3$s of object data; the index and FOREIGN KEY builds will read from disk instead of memory. Consider data_buffer_size=%4$s or more for this import.\n" },
  };
}

extern "C" const char *
importdb_msgcat_message (int cat_id, int set_id, int msg_id)
{
  (void) cat_id;
  (void) set_id;
  for (size_t i = 0; i < sizeof (importdb_messages) / sizeof (importdb_messages[0]); i++)
    {
      if (importdb_messages[i].id == msg_id)
        {
          return importdb_messages[i].text;
        }
    }
  return "importdb: message not found in the utility's own catalog\n";
}
