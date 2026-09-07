# The upstream contract

What this directory checks is the **installed** surface — `$CUBRID/include`,
`$CUBRID/lib/libcubridcs`, and `$CUBRID/bin` for the utilities the data phase
execs. `contract_check` itself is built against nothing else, which is why it
needs no source tree and can run against any install.

That is narrower than the repo. `cubrid-importdb` is compiled against the
engine's own headers out of a source tree and a configured build tree — see
[Requirements](../README.md#requirements) — so the installed surface is the
*runtime* dependency, not the whole of it. It is also the half that can quietly
break without anyone touching this repo.

So it is written down here, and machine-checked.

## Files

| file | what it is |
|---|---|
| `surface_manifest.txt` | the enumerated dependency: headers, declarations, unmangled exports, replicated constants, source-shape contracts |
| `check_surface.sh` | static half. `--install <dir>` against an install, `--source <dir>` against an engine source tree. Seconds, no build, no server |
| `contract_check.cpp` | runtime half. 16 checks against a live database: catalog reads, the SQL substitutes, the parameter reads, the `cub_admin` argv contract |
| `run.sh` | creates a scratch database + dump, runs `contract_check`, cleans up. `CONTRACT_NEGATIVE=1` runs it as a non-DBA, which must fail |

## Where this runs

Here, and only here. importdb is an extension of CUBRID, not a part of it, so
watching the engine is this repo's job and the engine repo carries nothing on
importdb's behalf.

Both workflows fetch the engine as a **(source, install) pair from one published
build**, which is what lets one lane run both halves of the check:

| | mode | what it reads | what it catches |
|---|---|---|---|
| `contract.yml` | `--source` + `--install` | `engine/src` and `engine/install` | daily, against the newest 11.5 nightly |
| `nightly.yml` | `--source` + `--install` | the same pair | the same, plus the functional suite and the runtime contract |

Source mode is the cheaper and stricter half: it reads the engine's install rules
and headers straight out of the tree, in seconds, and it is the only half that can
see a replicated constant change or a source-shape contract break. Install mode is
the only half that can see a symbol get mangled or a header stop being installed.
Neither subsumes the other, which is why the manifest marks each line's mode.

## Why the negative control exists

A check suite that can only pass proves nothing. `CONTRACT_NEGATIVE=1` runs the
identical checks as a user outside the DBA group; six of them must fail. If that
run ever *passes*, the suite has stopped discriminating and the green tick is
worthless. CI asserts the failure.

## What the manifest encodes, and why each line is there

- **headers / install** — the eight installed headers. `dbi.h` is installed as a
  rename of `src/compat/dbi_compat.h`, which is why source mode maps names.
- **decl** — the ~25 functions this repo actually calls. A declaration that
  disappears is a compile break; catching it statically names the symbol instead
  of dumping a compiler error.
- **export** — a handful of symbols checked for *unmangled* export. A mangled
  name is a contract break even when it links, because the mangling encodes the
  parameter types: the same function with one more argument is a different
  symbol, and the failure surfaces at load time in the field.
- **const** — `DB_CLIENT_TYPE_ADMIN_UTILITY = 7`. `src/compat/db_client_type.hpp`
  is not installed, so this repo replicates the value. Installing that enum
  upstream would delete this line.
- **grep** — contracts that are source shape rather than API. The `cub_admin`
  argv resolution is the one the data phase depends on: execing `cub_admin
  loaddb …` instead of `cubrid loaddb …` keeps the loader a *direct* child, which
  is what makes `PR_SET_PDEATHSIG` reach it when the parent is killed. Two more
  pin the pre-11.5 compatibility rule: `ldr_compat_call_target` in `load_db.c` is
  where the rule set is *defined*, and `CTV_SERIAL_NAME` in
  `schema_system_catalog.cpp` is why it is needed at all. importdb cannot reach a
  parse tree from the installed surface, so it replicates those rewrites
  textually — a fourth renamed catalog view upstream, or a dropped
  `find_user`/`login` exemption, would make that copy stale and break pre-11.5
  dumps again, silently. These are the lines that would notice.

## Adding a requirement

Add the line, run both modes locally, and say in the commit message which code
started depending on it. A manifest line with no caller is worse than no line —
it makes an upstream change look like a break when nothing would have noticed.
