/*
 * importdb_utility_ext.h -- what the utility repo owns.
 *
 * These declarations live in the engine's src/executables/utility.h today
 * because importdb put them there. They are importdb's own -- a message-set
 * number, its message ids, its CLI option constants, its entry point -- so in a
 * separate repo they belong here, and the engine's utility.h goes back to stock.
 *
 * Force-included (-include) ahead of utility.h so the 16 importdb sources need
 * no edits at all.
 */
#ifndef _IMPORTDB_UTILITY_EXT_H_
#define _IMPORTDB_UTILITY_EXT_H_

/* Before the split lands upstream, the engine's own utility.h still carries
 * these. The build detects that (see CMakeLists.txt) and defines
 * IMPORTDB_ENGINE_HAS_DECLS so this header stands down rather than colliding --
 * which lets the repo build against both a split and an unsplit engine. */
#ifndef IMPORTDB_ENGINE_HAS_DECLS

/* importdb's own utility name, as the CLI and the option tables use it */
#define UTIL_OPTION_IMPORTDB "importdb"

/* the message set number importdb claims in the shared utils catalog */
#define MSGCAT_UTIL_SET_IMPORTDB 61

/* Message id in the set MSGCAT_UTIL_SET_IMPORTDB */
typedef enum
{
  IMPORTDB_MSG_EXCEPTIONS_TABLE_RESERVED = 10,
  IMPORTDB_MSG_DEGREE_CLAMPED = 11,
  IMPORTDB_MSG_DISCOVERY_SUMMARY = 12,
  IMPORTDB_MSG_NO_DUMP = 13,
  IMPORTDB_MSG_NO_SCHEMA = 14,
  IMPORTDB_MSG_SCHEMA_INFO_MISSING = 15,
  IMPORTDB_MSG_PREFIX_MISMATCH = 16,
  IMPORTDB_MSG_NO_OBJECTS = 17,
  IMPORTDB_MSG_DIR_OPEN_FAILED = 18,
  IMPORTDB_MSG_MANIFEST_WRITE_FAILED = 19,
  IMPORTDB_MSG_CONNECT_FAILED = 20,
  IMPORTDB_MSG_NOT_DBA = 21,
  IMPORTDB_MSG_DEFINE_STMT_FAILED = 22,
  IMPORTDB_MSG_DEFINE_FILE_OPEN_FAILED = 23,
  IMPORTDB_MSG_DEFINE_COMPLETE = 24,
  IMPORTDB_MSG_GRAPH_SUMMARY = 25,
  IMPORTDB_MSG_OBJECT_CLASSES_REJECTED = 26,
  IMPORTDB_MSG_OBJECT_CLASSES_SKIPPED = 27,
  IMPORTDB_MSG_GRAPH_QUERY_FAILED = 28,
  IMPORTDB_MSG_SCHEDULE_SUMMARY = 29,
  IMPORTDB_MSG_SCHEDULE_INVALID = 30,
  IMPORTDB_MSG_DRY_RUN_COMPLETE = 31,
  IMPORTDB_MSG_STRIP_STMT_FAILED = 32,
  IMPORTDB_MSG_STRIP_COMPLETE = 33,
  IMPORTDB_MSG_LOAD_FAILED = 34,
  IMPORTDB_MSG_LOAD_COMPLETE = 35,
  IMPORTDB_MSG_REBUILD_CONSTRAINT_FAILED = 36,
  IMPORTDB_MSG_REBUILD_INDEX_FAILED = 37,
  IMPORTDB_MSG_REBUILD_FILE_OPEN_FAILED = 38,
  IMPORTDB_MSG_REBUILD_COMPLETE = 39,
  IMPORTDB_MSG_REBUILD_PARTIAL = 40,
  IMPORTDB_MSG_VALIDATE_EDGE_VIOLATED = 41,
  IMPORTDB_MSG_VALIDATE_COMPLETE = 42,
  IMPORTDB_MSG_VALIDATE_VIOLATIONS = 43,
  IMPORTDB_MSG_VALIDATE_QUERY_FAILED = 44,
  IMPORTDB_MSG_EXCEPTIONS_WRITE_FAILED = 45,
  IMPORTDB_MSG_FKDEFINE_COMPLETE = 46,
  IMPORTDB_MSG_FKDEFINE_WITHHELD = 47,
  IMPORTDB_MSG_FKDEFINE_FAILED = 48,
  IMPORTDB_MSG_FKDEFINE_FILE_OPEN_FAILED = 49,
  IMPORTDB_MSG_STATS_COMPLETE = 50,
  IMPORTDB_MSG_STATS_PARTIAL = 51,
  IMPORTDB_MSG_STATS_CLASS_FAILED = 52,
  IMPORTDB_MSG_TRIGGER_COMPLETE = 53,
  IMPORTDB_MSG_TRIGGER_PARTIAL = 54,
  IMPORTDB_MSG_TRIGGER_STMT_FAILED = 55,
  IMPORTDB_MSG_TRIGGER_FILE_OPEN_FAILED = 56,
  IMPORTDB_MSG_REPORT_HEADER = 57,
  IMPORTDB_MSG_REPORT_SUMMARY = 58,
  IMPORTDB_MSG_LOAD_PARALLEL_COMPLETE = 59,
  IMPORTDB_MSG_HA_REFUSED = 77,
  IMPORTDB_MSG_HA_PROCEEDING = 78,
  IMPORTDB_MSG_HA_SLAVE_DIVERGED = 79,
  IMPORTDB_MSG_USAGE = 60,
  IMPORTDB_MSG_OBJECT_CLASSES_SINGLE = 61,
  IMPORTDB_MSG_ALREADY_DONE = 62,
  IMPORTDB_MSG_RESUME_START = 63,
  IMPORTDB_MSG_RESUME_NOT_POSSIBLE = 64,
  IMPORTDB_MSG_RESTART_IGNORING = 65,
  IMPORTDB_MSG_TRUNCATE_FAILED = 66,
  IMPORTDB_MSG_RESUME_TRUNCATED = 67,
  IMPORTDB_MSG_CATALOG_QUERY_FAILED = 68,
  IMPORTDB_MSG_ALREADY_DONE_PARTIAL = 69,
  IMPORTDB_MSG_COMMIT_FAILED = 70,
  IMPORTDB_MSG_RESUME_TARGET_MISMATCH = 71,
  IMPORTDB_MSG_MANIFEST_PRESERVED = 72,
  IMPORTDB_MSG_MANIFEST_UNREADABLE = 73,
  IMPORTDB_MSG_MANIFEST_CORRUPT = 74,
  IMPORTDB_MSG_MANIFEST_PRESERVE_FAILED = 75,
  IMPORTDB_MSG_PARTIALLY_IMPORTED = 76,
  IMPORTDB_MSG_DEFINE_COMPAT_REWRITE = 81,
  IMPORTDB_MSG_SMALL_PAGE_BUFFER = 82,
  IMPORTDB_MSG_DEFINE_PRE115_HINT = 83,
  IMPORTDB_MSG_TARGET_NOT_EMPTY = 84
} MSGCAT_IMPORTDB_MSG;

/* importdb option list */
#define IMPORT_USER_S               'u'
#define IMPORT_USER_L               "user"
#define IMPORT_PASSWORD_S           'p'
#define IMPORT_PASSWORD_L           "password"
#define IMPORT_DEGREE_S             14201
#define IMPORT_DEGREE_L             "degree"
#define IMPORT_CONTINUE_S           14202
#define IMPORT_CONTINUE_L           "continue"
#define IMPORT_SKIP_OBJECT_CLASSES_S 14203
#define IMPORT_SKIP_OBJECT_CLASSES_L "skip-object-classes"
#define IMPORT_DRY_RUN_S            14204
#define IMPORT_DRY_RUN_L            "dry-run"
#define IMPORT_EXCEPTIONS_TABLE_S   14205
#define IMPORT_EXCEPTIONS_TABLE_L   "exceptions-table"
#define IMPORT_RESTART_S            14206
#define IMPORT_RESTART_L            "restart"
#define IMPORT_ALLOW_HA_S           14207
#define IMPORT_ALLOW_HA_L           "allow-ha"

/* NOTE: the importdb() entry is NOT declared here. This header is force-included
 * ahead of utility.h, so UTIL_FUNCTION_ARG does not exist yet -- the entry is
 * declared by the caller after its own includes. */

#endif /* !IMPORTDB_ENGINE_HAS_DECLS */

/* Added after the split, so no engine utility.h carries these -- they sit
 * OUTSIDE the stand-down guard above and are individually guarded instead, which
 * keeps the repo building against an engine that already has the older decls. */
#ifndef IMPORT_PROGRESS_S
#define IMPORT_PROGRESS_S           14208
#define IMPORT_PROGRESS_L           "progress"
#endif

#ifndef IMPORTDB_MSG_PROGRESS_INVALID
#define IMPORTDB_MSG_PROGRESS_INVALID 80
#endif

#endif /* _IMPORTDB_UTILITY_EXT_H_ */
