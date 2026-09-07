/*
 * importdb_main.cpp -- the out-of-tree entry point.
 *
 * In-tree, cub_admin looks `importdb` up in its own utility map and calls
 * util_parse_argument for us. A separate repo owns those two tables instead --
 * they are importdb's own, they just happen to live in the engine's
 * util_admin.c today -- and does the same two calls itself.
 */
#include "error_code.h"
#include "message_catalog.h"
#include "utility.h"
#include "util_func.h"
#include "util_support.h"
/* the engine's generated version header, out of the configured build tree that
 * cmake/EngineFlags.cmake already puts first on the include path. This is the
 * one honest answer to --version for a source distribution: not a release of
 * this repo, which has none, but the engine this binary was compiled against
 * and is therefore good for. */
#include "version.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* declared here rather than in the force-included header, which is seen before
 * utility.h and so cannot name UTIL_FUNCTION_ARG */
extern int importdb (UTIL_FUNCTION_ARG *arg_map);

static GETOPT_LONG oot_Import_Option[] = {
  {IMPORT_USER_L, 1, 0, IMPORT_USER_S},
  {IMPORT_PASSWORD_L, 1, 0, IMPORT_PASSWORD_S},
  {IMPORT_DEGREE_L, 1, 0, IMPORT_DEGREE_S},
  {IMPORT_CONTINUE_L, 0, 0, IMPORT_CONTINUE_S},
  {IMPORT_SKIP_OBJECT_CLASSES_L, 0, 0, IMPORT_SKIP_OBJECT_CLASSES_S},
  {IMPORT_DRY_RUN_L, 0, 0, IMPORT_DRY_RUN_S},
  {IMPORT_EXCEPTIONS_TABLE_L, 1, 0, IMPORT_EXCEPTIONS_TABLE_S},
  {IMPORT_RESTART_L, 0, 0, IMPORT_RESTART_S},
  {IMPORT_ALLOW_HA_L, 0, 0, IMPORT_ALLOW_HA_S},
  {IMPORT_PROGRESS_L, 1, 0, IMPORT_PROGRESS_S},
  {0, 0, 0, 0}
};

static UTIL_ARG_MAP oot_Import_Option_Map[] = {
  {OPTION_STRING_TABLE, {0}, {0}},
  {IMPORT_USER_S, {ARG_STRING}, {0}},
  {IMPORT_PASSWORD_S, {ARG_STRING}, {0}},
  {IMPORT_DEGREE_S, {ARG_INTEGER}, {(void *) 1}},
  {IMPORT_CONTINUE_S, {ARG_BOOLEAN}, {0}},
  {IMPORT_SKIP_OBJECT_CLASSES_S, {ARG_BOOLEAN}, {0}},
  {IMPORT_DRY_RUN_S, {ARG_BOOLEAN}, {0}},
  {IMPORT_EXCEPTIONS_TABLE_S, {ARG_STRING}, {0}},
  {IMPORT_RESTART_S, {ARG_BOOLEAN}, {0}},
  {IMPORT_ALLOW_HA_S, {ARG_BOOLEAN}, {0}},
  {IMPORT_PROGRESS_S, {ARG_STRING}, {0}},
  {0, {0}, {0}}
};

static UTIL_MAP oot_Import_Map[] = {
  {0, CS_ONLY, 2, UTIL_OPTION_IMPORTDB, "importdb", oot_Import_Option, oot_Import_Option_Map},
  {-1, -1, 0, 0, 0, 0, 0}
};

/*
 * --help and --version are answered here, before util_parse_argument, and
 * deliberately not as entries in the option tables above. Both are "print and
 * exit 0" with no database and no dump directory, which is precisely the shape
 * util_parse_argument rejects -- routing them through it would make asking for
 * help indistinguishable from the usage error help exists to prevent.
 *
 * Until now there was no way to ask at all. The only route to the usage text was
 * to invoke the tool wrongly, which prints it to stderr and exits 1: a script
 * doing `cubrid-importdb --help` got a failure and an empty stdout.
 */
static bool
answer_help_or_version (int argc, char **argv)
{
  for (int i = 1; i < argc; i++)
    {
      if (std::strcmp (argv[i], "--") == 0)
	{
	  break;			/* everything after this is positional */
	}
      if (std::strcmp (argv[i], "-h") == 0 || std::strcmp (argv[i], "--help") == 0)
	{
	  /* stdout, not stderr: `cubrid-importdb --help | less` must show it */
	  std::printf (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
				       IMPORTDB_MSG_USAGE), basename (argv[0]));
	  return true;
	}
      if (std::strcmp (argv[i], "--version") == 0)
	{
	  std::printf ("cubrid-importdb (built against CUBRID %s)\n", VERSION_STRING);
	  return true;
	}
    }
  return false;
}

int
main (int argc, char **argv)
{
  /* Line-buffer stdout even when it is not a terminal. The phase summaries go to
   * stdout and the diagnostics to stderr, and stderr is unbuffered: with the
   * default block buffering, `cubrid-importdb ... > import.log 2>&1` flushes
   * stdout only when its 4 KB buffer fills, so every error in the log appears
   * ABOVE the phase it belongs to -- and the first thing an operator reads is a
   * rejection with no context. Interactive runs already behaved correctly; this
   * makes a redirected run read the same way. */
  setvbuf (stdout, NULL, _IOLBF, 0);

  if (answer_help_or_version (argc, argv))
    {
      return EXIT_SUCCESS;
    }

  UTIL_FUNCTION_ARG arg;
  bool valid = (util_parse_argument (&oot_Import_Map[0], argc, argv) == NO_ERROR);

  std::memset (&arg, 0, sizeof (arg));
  arg.command_name = (char *) UTIL_OPTION_IMPORTDB;
  arg.arg_map = oot_Import_Option_Map;
  arg.argv0 = argv[0];
  arg.argv = argv;
  arg.valid_arg = valid;

  return importdb (&arg);
}
