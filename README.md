# cubrid-importdb

`cubrid importdb` as an out-of-tree utility: it builds against an **installed**
CUBRID (`$CUBRID/include` + `libcubridcs` + `$CUBRID/bin`) and never references a
CUBRID source tree.

Status: **contract and build scaffolding**. The utility's own sources have not
been ported yet — see [Porting](#porting).

## Why this can exist

importdb consumes 36 internal CUBRID calls plus two authorization macros. Of
those, 28 are already declared by the headers CUBRID installs; the rest have
substitutes that were measured to work:

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

## Porting

What remains is mechanical but not trivial: 16 source files, ~10,000 lines, whose
non-installed includes have to be redirected. Per-file dependency counts (from
the engine tree):

| non-installed header | files | replacement |
|---|---|---|
| `utility.h`, `message_catalog.h`, `util_func.h` | 16 / 16 / 14 | this repo's message table (70 ids, message set 61) |
| `db.h`, `dbtype.h` | 11 / 4 | `dbi.h` + `dbtype_function.h` (installed) |
| `authenticate.h` | 10 | removed (see the table above) |
| `error_manager.h` | 7 | `error_code.h` (installed) + `db_error_code ()`; no `er_*` call sites exist |
| `porting.h` | 3 | not needed on Linux |
| `environment_variable.h` | 2 | `$CUBRID` path composition |
| `system_parameter.h`, `connection_defs.h`, `config.h`, `boot.h` | 1 each | `db_get_system_parameters ()`; the last two have no call sites — dead includes |
| `network_interface_cl.h` | 1 | no call sites — dead include |
| `schema_manager.h`, `statistics.h` | 1 | SQL |
| `db_client_type.hpp` | 1 | literal 7, see the manifest `const` line |

`boot.h`, `network_interface_cl.h`, `config.h` have **no matching call sites** in
`src/importdb/` — they are leftover includes, so three of the sixteen are free.
