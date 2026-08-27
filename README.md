# cubrid-importdb

`cubrid importdb` as an out-of-tree utility, distributed as **source** and
recompiled against the engine it will run with.

Status: **builds and imports.** `tools/smoke.sh` runs a real two-table import
(PK, FK, plain index, 450 rows) and checks the catalog round-trip.

## What is distributed, and what that costs

**Source, not binaries.** The utility is recompiled against the engine it will
run with, so there is no ABI to keep stable — only an API. That single decision
is why the split is nearly free: **the 16 importdb sources move byte-identical,
with no edits at all.**

What this repo owns instead:

| file | lines | what it is |
|---|---|---|
| `include/importdb_utility_ext.h` | ~120 | importdb's own declarations — the utility name, the message-set number, its 70 message ids, its 19 CLI option constants. They sit in the engine's `src/executables/utility.h` today only because importdb put them there |
| `src/importdb_messages.cpp` | 111 | the 70 message strings, generated from `msg/en_US.utf8/utils.msg` set 61. In-tree they are compiled into `$CUBRID/msg/*/utils.cat`, which a separate repo cannot write |
| `src/importdb_main.cpp` | ~60 | the entry point, plus the two option tables that live in the engine's `util_admin.c` today |
| `cmake/EngineFlags.cmake` | 55 | the engine's 39 include dirs and 13 defines, generated from its own compile line |

And one build define does the work that would otherwise mean editing 16 files:

```
-Dmsgcat_message=importdb_msgcat_message
```

Applied before any header, it renames the declaration in `message_catalog.h` **and
every call site** together. That is the whole trick.

The build also compiles the engine's own `src/executables/util_support.c` (389
lines) because `util_parse_argument` is not in `libcubridcs` — borrowed, not
copied, which is a thing only a source distribution can do.

**And the engine gets smaller.** `utility.h`, `util_admin.c`, `msg/*/utils.msg`
and `cs/CMakeLists.txt` all go back to stock: importdb stops modifying the engine
at all. Verified by compiling the 16 sources against the pre-importdb `utility.h`
— 16/16, zero edits. Until that engine-side change lands, the build detects that
the engine still declares importdb and this repo's declarations stand down, so it
works against a split and an unsplit engine both.

## Build modes

| mode | needs | builds |
|---|---|---|
| `-DCUBRID_SOURCE_DIR=<engine src> -DCUBRID_BUILD_DIR=<configured engine build>` | an engine **source** tree | `cubrid-importdb`, the real thing |
| `-DBUILD_UTILITY=OFF` | only `$CUBRID` (an **install**) | `contract_check`, the cheap behaviour probe |

```sh
export CUBRID=/path/to/install
cmake -S . -B build -DCUBRID_SOURCE_DIR=/path/to/cubrid -DCUBRID_BUILD_DIR=/path/to/cubrid/build
cmake --build build -j
CUBRID=$CUBRID bash tools/smoke.sh build/cubrid-importdb     # a real import
CONTRACT_CHECK_BIN=$PWD/build/contract_check bash contract/run.sh
```

## The 8 calls the installed headers do not declare

Relevant only to the `contract_check` half, which builds against an install. The
utility itself sees the engine's own headers, so it needs none of these
substitutes — but they are what the contract check asserts still work, because
they are the assumptions the code rests on:

| was | now |
|---|---|
| `sm_update_statistics` | SQL `UPDATE STATISTICS ON <cls> [WITH FULLSCAN]` / `ON ALL CLASSES` |
| `prm_get_integer_value` | `db_get_system_parameters ()` (installed) |
| `HA_DISABLED ()` macro | the same function, asking the server for `ha_mode` |
| `au_is_dba_group_member (Au_user)` | read `db_user.groups` as a SET and iterate it |
| `AU_SAVE_AND_DISABLE` / `AU_RESTORE` | nothing — a DBA or DBA-group member does every one of these operations with authorization *enabled*, and dropping the window means the engine enforces it instead of one connect-time check |
| `msgcat_message` / `envvar_bindir_file` / `util_log_write_errid` | this repo's own message table, `$CUBRID/bin` path composition, own logging |
| the `loaddb -C` subprocess pool | `fork` + `PR_SET_PDEATHSIG` + `exec $CUBRID/bin/cub_admin loaddb -C` |

The measurement behind that table is in the vault:
`plan/importdb/NOTES_out_of_tree_feasibility.md`.

## Build

```sh
export CUBRID=/path/to/cubrid            # an install, not a source tree
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
CONTRACT_CHECK_BIN=$PWD/build/contract_check bash contract/run.sh
```

`FindCUBRID.cmake` fails the configure step with a named header if the install is
missing one, rather than letting it become a compile error.

## The contract with upstream

The whole dependency on CUBRID is enumerated in
[`contract/surface_manifest.txt`](contract/surface_manifest.txt) and checked two
ways — statically (no build, no server) and at runtime against a live database.
See [`contract/README.md`](contract/README.md). CI runs both, and runs a
**negative control** that must fail, so a green tick means the suite still
discriminates.

## What can break, and what catches it

| breakage | caught by |
|---|---|
| a header it includes is removed or renamed | compile |
| a function it calls is removed or renamed | compile |
| a signature changes with no compatible default | compile |
| a type it uses changes shape | compile |
| `utility.h`'s option/arg machinery changes | compile |
| `util_support.c` moves | build |
| authorization tightened on a catalog class, an SQL form withdrawn, the `cub_admin` argv contract changed | **only** the smoke test / `contract_check` |

So the guard is the build plus one real import — not an ABI check. CI is split on
exactly that line: compile on every engine PR (~2 min, no engine build, the
configured build tree is cached for its generated headers), link + import
nightly. See [`contract/README.md`](contract/README.md) and the two workflows.

A signature change *with* a compatible default is benign here — which is a point
in favour of source distribution, and was verified the hard way: building against
the merge-base tree with a newer library produced exactly the failure a binary
distribution would suffer (`sm_update_statistics(db_object*, bool)` unresolved
against a library exporting the three-argument form), and it disappears when
source and library come from the same commit.

## Porting — what is left

The 16 sources need nothing. What remains:

- **the serial data path.** `import_load.cpp` drives loaddb in process via
  `loaddb_init` / `loaddb_load_batch`. In the source model that is *fine* — the
  headers are right there — so this is no longer a blocker, but it does mean the
  utility keeps reaching into `src/loaddb`, which the CI path filter reflects.
- **the CLI surface.** `cubrid importdb …` becomes `cubrid-importdb …` unless the
  engine keeps its one row in the utility map. Worth a decision, not work.
- **the engine-side removal PR**, which is what makes the split real rather than
  additive.
