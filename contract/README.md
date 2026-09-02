# The upstream contract

This repo builds **only** against what CUBRID installs — `$CUBRID/include`,
`$CUBRID/lib/libcubridcs`, and `$CUBRID/bin` for the utilities the data phase
execs. It never references a CUBRID source tree. That is what makes an
independent repo possible, and it is also the thing that can quietly break.

So the dependency is written down and machine-checked.

## Files

| file | what it is |
|---|---|
| `surface_manifest.txt` | the enumerated dependency: headers, declarations, unmangled exports, replicated constants, source-shape contracts |
| `check_surface.sh` | static half. `--install <dir>` against an install, `--source <dir>` against an engine source tree. Seconds, no build, no server |
| `contract_check.cpp` | runtime half. 16 checks against a live database: catalog reads, the SQL substitutes, the parameter reads, the `cub_admin` argv contract |
| `run.sh` | creates a scratch database + dump, runs `contract_check`, cleans up. `CONTRACT_NEGATIVE=1` runs it as a non-DBA, which must fail |

## The two lanes

**This repo** (`.github/workflows/contract.yml`) runs both halves against the
CUBRID installs it claims to support. Catches drift even when nobody touches
either repo.

**The engine repo** (`.github/workflows/upstream-guard.yml.for-cubrid-repo`,
copy it there) runs the static half on every PR that touches a path which could
move the surface — about 30 seconds, no build — and the runtime half nightly,
where a build already has to happen. An engine change that breaks the utility
fails on the engine PR, which is the only place it is cheap to fix.

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
