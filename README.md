![CUBRID ImportDB](assets/banner.svg)

*English · [한국어](README.ko.md)*

# CUBRID ImportDB

**A one-command reloader for CUBRID `unloaddb` dumps.** It reads the dump directory,
works out the load order from the schema's own dependency graph, loads the data into
constraint-free heaps, and rebuilds every index and foreign key afterwards. That makes
the reload faster than the schema-first script it replaces — and it makes the engine
actually check the foreign keys, which that script never does.

```
cubrid-importdb -u dba newdb /path/to/dump
```

That is the entire operator interface for a full-database reload: point it at the
directory `unloaddb` wrote, and it works out the rest.

**Status.** This is an **unofficial** tool. It is not part of the CUBRID
distribution, it is not endorsed by the CUBRID project, and **it has not been
validated in production**. The evidence that it works is the suites in
[`tests/`](tests/README.md), [`demo/`](demo/README.md) and
[`contract/`](contract/README.md), and nothing beyond them. Evaluate it against a
copy of your data before you point it at anything you would miss.

It is a separate binary rather than a `cubrid` subcommand because it is built
*out of tree* — against the engine rather than inside it, so it can be developed
and released without touching CUBRID itself. See
[docs/out-of-tree.md](docs/out-of-tree.md).

**Before you start**, four facts decide whether this is usable where you are:

| | |
|---|---|
| **Target engine** | **CUBRID 11.5 or newer** — currently the nightly series, so a released 11.4 or earlier target will not work at all. The dump it *reads* may come from 10.2 up; the two versions are independent. See [Requirements](#requirements). |
| **The binary** | Built from source against the engine it will run with. There is no binary release and no `cmake --install` target, so `cubrid-importdb` is on your `PATH` only if you put it there — see [Install](#install). Until then it is `build/cubrid-importdb`. |
| **The target database** | Yours to create, already **running**, and holding **no user class**. importdb imports *into* an existing database; it never creates one. See [Your first import](#your-first-import). |
| **The dump directory** | Must be **writable**. The resume manifest — and, on a violation, the exceptions artifact — are written next to the dump, so a read-only NFS export of a dump does not work as-is. |

**In a hurry?** [Requirements](#requirements) → [Install](#install) →
[Your first import](#your-first-import). Everything before those three is the
argument for the tool, and you can skip it.

<details>
<summary>Contents</summary>

**Why** — [The problem](#the-problem) · [Features](#features) ·
[How it works](#how-it-works) · [Referential integrity](#referential-integrity) ·
[Reading an older engine's dump](#reading-an-older-engines-dump) ·
[Performance](#performance)

**Using it** — [Requirements](#requirements) · [Install](#install) ·
[Your first import](#your-first-import) · [Usage](#usage) ·
[What it does not do](#what-it-does-not-do)

**Working on it** — [Development](#development) · [License](#license)

</details>

---

## The problem

`unloaddb` hands you a directory. Reloading it is on you:

```sh
cubrid loaddb -C -u dba -s mydb_schema  newdb   # schema first, so every
cubrid loaddb -C -u dba -d mydb_objects newdb   # PK/UNIQUE/FK is live and
cubrid loaddb -C -u dba -i mydb_indexes newdb   # maintained per row
```

Three invocations for a small database, and the ordering is yours to get right.
That costs two things:

**Speed.** `unloaddb` writes primary keys, unique constraints **and foreign keys**
into `<prefix>_schema`, so schema-first means every constraint is live for the
entire data phase and maintained one row at a time. Only the plain secondary
indexes are deferred to `_indexes`.

**Correctness, and this is the one that matters.** `loaddb` turns foreign-key
checking *off* for the data phase and never turns it back on to re-check. Load a
dump containing an orphan row and it succeeds, reports nothing, and leaves you a
database where the FOREIGN KEY is present in the catalog **and violated by the
data** — a state the engine itself would refuse to create. You find out later,
from a wrong query answer.

CUBRID ImportDB replaces that with a single command that plans the load from the
dependency graph, loads into bare heaps and bulk-builds the constraints
afterwards, and refuses to define a foreign key the data does not satisfy —
telling you exactly which rows are at fault.

![The constraint lifecycle, two ways](assets/lifecycle.svg)

*Figure 1 — the same twelve rows, two ways. Live constraints cost one b-tree
write per row, per index, and still leave the foreign keys unchecked; stripping
them and building afterwards makes the build itself the check — which is why the
orphan is caught in one lane and silently kept in the other.*

## Features

**One command for the whole dump.** Discovers the schema, object and index files,
and the layout they were written in — default single-file or `--datafile-per-class`.
No ordering for you to get right.

**Dependency-aware planning.** Reads the catalog after defining the schema and
builds a graph from FK, inheritance, partitioning and serials, then loads in level
order. FK definition is deferred for every edge, so mutually-referencing tables
need no special handling — a cycle loads in one pass like anything else.

**Heap-only load.** Strips PK/UNIQUE/FK, loads into bare heaps, then bulk-builds
the constraints on the populated tables. No per-row index maintenance during the
data phase.

**Referential integrity enforced, not assumed.** Every foreign key is defined
against the loaded data. If the data violates one, importdb enumerates *every*
offending row — the engine names only the first — and withholds that FK rather
than defining a broken one.

**The verdict is checked, not assumed.** Before a run calls itself complete, the
catalog is read back and every constraint the snapshot named must actually be
there or be recorded as withheld. The phase counters are importdb's own
bookkeeping; this is the only step that can catch them being wrong about the
run, and a mismatch makes the verdict PARTIAL and names what is missing.

**A repair record you can act on.** `importdb.exceptions` lists each orphan by its
primary key (or by position, for a child table without one) and carries the exact
DDL to add the withheld FK once the data is fixed.

**A live display.** On a terminal, a progress block shows the current phase, its
position in the pipeline, and how far each loader has read into its object file.
Off automatically when stdout is not a terminal.

**Inter-table parallelism.** `--degree=N` runs N loaders concurrently, one per
object file; it needs a `--datafile-per-class` dump to have anything to spread over.

**Reads a dump from an older engine.** 10.2 and newer. A pre-11.5 `unloaddb`
names the catalog *views* as `CALL ... ON CLASS` targets, and 11.5 made those
views distinct from the classes that carry the methods; importdb rewrites the
three affected targets and says so, the way `loaddb` does. Without it every
10.2 dump of a database that owns a serial is refused.

**Resume.** A killed import re-run with the same command continues from its
manifest instead of starting over.

**Honest about HA.** Refuses an `ha_mode=on` target by default, because the
bare-heap load does not replicate to the standby. `--allow-ha` overrides and says
so twice — once before the load and once in the verdict.

**`--dry-run`.** Prints the plan and the terminal task order; changes nothing.

## How it works

**Discover → Define → Graph → Plan → Strip → Load → Rebuild → FK define → Stats →
Triggers.** The shape that matters is the middle: the schema is defined, its
constraint set is snapshotted and then *stripped*, the data goes into bare heaps,
and the constraints are bulk-built afterwards. Triggers are defined strictly last,
so nothing fires during the load.

![The ten phases, and where the time goes](assets/phases.svg)

*Figure 2 — the pipeline, numbered the way the progress block numbers it. The
bars are each phase's measured share of a 3.93M-row run: the two phases that
build constraints are 80% of it, and both are serial.*

The first four phases are the ones the hand-scripted path has no equivalent for.
`loaddb` takes its order from whoever typed the three invocations. importdb reads
the target catalog once the schema is defined and derives one — which is also why
a cycle is not a special case: FK definition is deferred for every edge, so two
tables that reference each other land in the same level and load in one pass.

![The load order is read from the schema, not typed](assets/plan.svg)

*Figure 3 — Discover, Define, Graph and Plan on the shop fixture. The graph, the
three level sets and the seventeen terminal tasks are the run's own, printed in
[docs/output.md](docs/output.md); the cycle in the note is
`tests/fixtures/fkcycle.sql`, where `cy_a` and `cy_b` reference each other and no
load order satisfies both FKs.*

Foreign keys are defined against the loaded data, which is where the engine
validates them — `ADD CONSTRAINT ... FOREIGN KEY` builds the FK's b-tree over the
existing rows and checks each key against the parent as it goes. importdb does not
duplicate that work; it only takes over where the engine stops.

Each phase announces what it did. A clean run, with the graph and the schedule it
also prints left out:

```
importdb: rostered default dump (prefix 'shop') from /tmp/rmcap/dump
importdb: defined 'shoptgt'.
importdb: stripped 11 constraint(s) from 'shoptgt'.
importdb: loaded 3008 row(s) from 1 object file(s) into 'shoptgt'.
importdb: rebuilt 8 constraint(s) and built 2 index(es) on 'shoptgt'.
importdb: every FOREIGN KEY on 'shoptgt' was accepted by the engine -- 3 edge(s) clean, 0 skipped (parent key withheld).
importdb: defined 3 FK(s) on 'shoptgt'.
importdb: updated statistics on 5 class(es) in 'shoptgt'.
```

The run ends with a consolidated report — the per-class verdict, the row counts,
and anything left for you to repair. On a terminal those lines scroll past under a
block redrawn in place, which answers what the printed lines cannot: which phase
is running, how many are left, and how far into it you are.

```
 importdb  tuitgt                                           load  [6/10]  00:00
  ███████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  23%  0/4 done · 2 loading
    tuisrc_dba.customer         ██████████████████████████████░░░░░  85%  2 MB
    tuisrc_dba.orders           ██████░░░░░░░░░░░░░░░░░░░░░░░░░░░░  17%  18 MB
```

The per-file bars are real, not estimated: the loaders are separate
`cub_admin loaddb -C` processes — `cub_admin` is the binary behind `cubrid
loaddb`, and importdb execs it directly rather than going through the `cubrid`
front end — that report nothing until they exit, so importdb reads each one's
file offset out of `/proc/<pid>/fdinfo`. The display is off
whenever stdout is not a terminal, so a pipe, a file or a CI log gets exactly the
plain output it always got.

The tail of the pipeline is the other place the two paths part. `loaddb` updates
the per-class statistics at the end of the *object* load; the index file loaded
afterwards refreshes only the `_db_index` and `_db_indexkey` **catalog**
statistics, so the class statistics it computed predate the secondary indexes it
then creates. What that costs a query plan is not something measured here.
importdb removes the question instead of answering it: `--no-statistics` on every
loader child, and `STATS` scheduled per class after that class's rebuilds *and*
the index build — `#12 STATS audit_log [after #0, #8]`, where `#8` is the index
build.

![The statistics are computed last, and the run shows it](assets/stats.svg)

*Figure 4 — when the per-class statistics are computed in each path, and the two
surfaces a run shows: the block redrawn in place, and the phase lines that scroll
under it.*

Everything the dump defines is replayed from the dump's own DDL, which is more
than it looks: **users, their password hashes and their grants all come back**.

See [docs/output.md](docs/output.md) for the complete run with its graph and
schedule, the report, the exceptions artifact and how the passwords travel.

## Referential integrity

This is the difference worth showing. Take a dump with two orphan `orders` rows
whose `customer_id` does not exist.

**The hand-scripted `loaddb` path** accepts it, exit 0, nothing reported — the
FOREIGN KEY ends up in the catalog with data that violates it. Ask the engine to
create that same FK over that data and it refuses; `loaddb` left a state the
engine would not have allowed.

**CUBRID ImportDB** on the same dump loads the data, defines the clean FKs,
withholds the violated one, and exits 1:

```
importdb: stripped 11 constraint(s) from 'shopbad'.
importdb: loaded 3010 row(s) from 5 object file(s) into 'shopbad'.
importdb: rebuilt 8 constraint(s) and built 2 index(es) on 'shopbad'.
importdb: the engine rejected the FOREIGN KEY and found 2 orphan row(s) on 'orders' -> 'customer' [fk_orders_customer]; that FK is withheld and its offenders enumerated.
importdb: 'shopbad' has referential violations -- 1 FOREIGN KEY(s) rejected by the engine, 2 orphan row(s) total; offenders written to /tmp/rmcap/perclass/importdb.exceptions. See the manifest [validate] section; importdb exits non-zero.
importdb: defined 1 FK(s), withheld 2 on 'shopbad' (the engine rejected the data, or the parent key was un-rebuilt); re-add DDL recorded in the manifest [fkdefine] section and /tmp/rmcap/perclass/importdb.exceptions. importdb exits non-zero.
importdb: updated statistics on 5 class(es) in 'shopbad'.
```

and enumerates every offender into `importdb.exceptions`, with the DDL to add the
withheld FK once the data is fixed — see
[docs/output.md](docs/output.md#the-exceptions-artifact). The engine names only
the first offending value and stops. `--continue` attempts every edge instead of
stopping at the first.

`demo/run_demo.sh` runs both paths side by side — see [`demo/`](demo/README.md).

## Reading an older engine's dump

A reload is usually a *migration*: the dump is written by the version you are
leaving and read by the version you are arriving at. importdb reads dumps from
**CUBRID 10.2 and newer**, and the reason it can is mostly structural — the graph,
the plan, the strip, the rebuild and the FK definition are all driven by a catalog
read of the target, not by parsing the dump's DDL.

What is not structural is the dump's own text. It is fed to the engine's own
parser, so importdb inherits `loaddb -s`'s tolerance for old syntax exactly — with
one exception it had to fix, because a pre-11.5 `unloaddb` names the catalog
*views* as `CALL ... ON CLASS` targets and 11.5 moved those methods. Every 10.2
dump of a database that owns a serial carries one, so the whole class of dumps was
refused until importdb started rewriting them.

`tests/run_tests.sh crossversion` is the evidence, on a real 10.2 install: the
data compared against the 10.2 source, and the catalog and the grant set against a
target built from a *current* dump of the same fixtures, all byte-identical.

See [docs/old-dumps.md](docs/old-dumps.md) for the floor's reasoning, the shapes
that are guarded, and the two that cannot be.

## Performance

Measured against a **parallelism-matched** `loaddb` baseline — the baseline fans
out over the same object files with the same bound, so the comparison isolates the
constraint lifecycle rather than crediting importdb with the concurrency.
`tests/run_tests.sh perf` **is** the measurement; there is no separate benchmark.

| degree | loaddb | importdb | median ratio | speedup |
|---|---|---|---|---|
| **1** | 5.70 s | 2.68 s | 0.481 &nbsp;(pairs 0.444 – 0.677) | **2.13× faster** |
| **4** | 5.08 s | 1.67 s | 0.362 &nbsp;(pairs 0.327 – 0.410) | **3.04× faster** |

393,000 rows over five object files, medians of five counted pairs, on one 16-core
Linux host. A ratio and not a stopwatch on purpose: the same baseline work has been
measured at 3.59 s and 6.07 s within a single run. Measure your own hardware.

Three things worth knowing before you rely on it:

- **Small dumps do not benefit.** The fixed setup — catalog snapshot, strip,
  rebuild, a child process per file — is paid whatever the row count.
- **`--degree` needs a `--datafile-per-class` dump.** The fan-out is over object
  files, so a default single-file dump runs serially whatever you pass — the run
  says so rather than leaving you to work it out from the timings.
- **At ten times this size the shape changes.** FK definition becomes half the run
  and `--degree` stops helping, and the cause is `data_buffer_size` rather than
  anything importdb does. importdb warns when the page buffer is small for the
  dump; the numbers are in the page below.

See [docs/performance.md](docs/performance.md) for the method, the phase-by-phase
breakdown at 3.93M rows, the page-buffer knee, and the three explanations that
measured out as wrong.

## Requirements

- **CUBRID 11.5 or newer** — for the engine importdb *runs against*. Read this
  one first: **11.5 is not a released series yet**, so the engine you already run
  in production almost certainly cannot be an importdb target. `tools/fetch_engine.sh`
  defaults to the 11.5 *nightly* for exactly this reason, and 11.4 is reachable
  only as `--release 11.4_latest`, which is a source of dumps and not a target.
  The dump importdb *reads* is a separate question and a much older floor — 10.2
  or newer; see
  [Reading an older engine's dump](#reading-an-older-engines-dump). importdb reads
  the `_db_serial` system class to find AUTO_INCREMENT serials (the `db_serial`
  view omits them), and that read is rejected on 11.4 and earlier. This is not enforced by a version check: on an
  older engine the `_db_serial` probe in [`contract/`](contract/README.md) fails,
  and an actual import fails in the graph phase.
- Linux, a C++17 compiler, and CMake 3.16+ for importdb itself.
- To *configure* the CUBRID engine (which the build below does, for its generated
  headers) you also need Ninja or Make, a JDK, bison, flex, ncurses and
  `dtrace` — the set CI installs is `cmake ninja-build gcc g++ libncurses-dev
  bison flex openjdk-17-jdk systemtap-sdt-dev`. Neither of the last two is
  optional, and neither is obvious: the engine defaults `ENABLE_SYSTEMTAP` on and
  its CMake stops without `dtrace`, and its `find_package(JNI REQUIRED)` wants
  AWT, which a *headless* JDK does not ship.
- A CUBRID **source tree** and a configured **build tree** to compile against, plus
  an **installed** CUBRID to link and run against. See [Install](#install).

  This is the bar most CUBRID users are furthest from: a tarball or RPM install
  is not enough on its own, because importdb's translation units include the
  engine's *generated* headers, which only a configured build tree produces. You
  do not have to build the engine — configuring it and building five header
  producers is about 20 seconds — but you do have to have its source. If all you
  have is an install, fetch the **matching** source rather than a similar one:
  `tools/fetch_engine.sh` pulls the source and install tarballs of the *same*
  build from `ftp.cubrid.org` and records the version and commit it took in
  `<dest>/engine.env`, so the pair cannot skew.

## Install

This is a source distribution: you build it against the CUBRID it will run with.
There is no binary release, and that is deliberate — see
[docs/out-of-tree.md](docs/out-of-tree.md).

The quickest path is the nightly drop, which publishes the source and the install
as a matched pair from the same build:

```sh
git clone https://github.com/cubrid-systems/cubrid-importdb
cd cubrid-importdb

# fetch a CUBRID source tree + install from ftp.cubrid.org (same build, no skew)
tools/fetch_engine.sh --nightly 11.5 engine

# the utility's translation units need the engine's generated and 3rdparty
# headers, which come from a CONFIGURED build tree -- not from a built engine
cmake -S engine/src -B engine/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build engine/build --target rapidjson re2 lz4 libexpat libjansson

export CUBRID="$PWD/engine/install"
cmake -S . -B build -DCUBRID_SOURCE_DIR="$PWD/engine/src" -DCUBRID_BUILD_DIR="$PWD/engine/build"
cmake --build build -j"$(nproc)"

build/cubrid-importdb --help     # prints the usage
build/cubrid-importdb --version  # the CUBRID it was built against
```

Configuring the engine and building only its header producers takes about 20
seconds. The engine itself is never built.

**The build stops at `build/cubrid-importdb`.** There is no `cmake --install`
target and nothing puts the binary on your `PATH`, so the short
`cubrid-importdb ...` form used throughout this README is the one you get after
you install it yourself. Next to the engine's own utilities is the tidiest
place, since everything that runs importdb already has that directory on `PATH`:

```sh
cp build/cubrid-importdb "$CUBRID/bin/"    # now `cubrid-importdb` resolves
cubrid-importdb --version
```

Skip that and every example below still works with `build/cubrid-importdb`
spelled out, or with `$IMPORTDB/build/cubrid-importdb` from a directory that is
not the clone. Note that a later `tools/fetch_engine.sh` into the same `engine/`
replaces the install tree and takes the copied binary with it.

Against a CUBRID you already have, point `CUBRID_SOURCE_DIR` and `CUBRID_BUILD_DIR`
at your own trees and `$CUBRID` at the install. If you have the install but no
source — the normal case for a tarball or RPM deployment — fetch the source of
that same build rather than building against a near-miss:

```sh
cubrid_rel     # CUBRID 11.5.0 (11.5.0.2513-5f3a30d) (64bit release build ...

# the parenthesised build string is the --nightly-version argument verbatim
tools/fetch_engine.sh --nightly-version 11.5.0.2513-5f3a30d engine
cmake -S engine/src -B engine/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build engine/build --target rapidjson re2 lz4 libexpat libjansson
```

`--release <NAME>` takes a release area (`11.4_latest`) and `--install-only`
skips the source half; `<dest>/engine.env` records the `VERSION` and `COMMIT`
that were actually taken, which is the thing to compare against your install
when a link or a runtime symbol looks wrong.

`$CUBRID` must be set at configure
time: the build bakes `$CUBRID/lib` and `$CUBRID/cci/lib` into the binary's
`RUNPATH`, because `libcubridcs` depends on `libcascci` from a directory nothing
else puts on the loader path.

> The build also reads the engine library's libstdc++ ABI with `nm` and matches it.
> Published CUBRID binaries link libstdc++ statically and use the pre-C++11
> `std::string` ABI, so `libcubridcs` re-exports those symbols and this binary
> resolves its own strings against them; a modern compiler defaults to the other
> ABI. You should never have to think about this, but if a link ever fails on
> `std::basic_string` symbols, `-DFORCE_OLD_CXX_ABI=ON` is the override.

## Your first import

Nothing above creates a database. importdb imports *into* a target that already
exists, is running, and holds no user class — so that target is yours to make,
and the dump is written by the source database's own engine, not by this tool.

```sh
# The runtime environment. Not the same as the build: importdb execs
# `cub_admin loaddb` itself, so the children need the engine on PATH and on the
# loader path, and every command below needs the same CUBRID_DATABASES. Note
# that the PATH export below is for the ENGINE's utilities -- it does not put
# cubrid-importdb anywhere; see Install for that.
IMPORTDB="$PWD"                                  # this clone, for the binary below
export CUBRID="$PWD/engine/install"
export CUBRID_DATABASES="$HOME/cubrid-databases" # holds databases.txt, the registry
export PATH="$CUBRID/bin:$PATH"
export LD_LIBRARY_PATH="$CUBRID/lib:$CUBRID/cci/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$CUBRID_DATABASES" && cd "$CUBRID_DATABASES"

# The dump. -S needs the source server stopped, and unloaddb writes into the
# current directory, so give it one of its own.
mkdir -p /dumps/mydb
( cd /dumps/mydb && cubrid unloaddb -S -u dba mydb )

# The target. createdb writes its volumes into the current directory and
# registers that path in databases.txt, so run it where the volumes should live
# -- not in the clone. Give it the same locale as the source database, because
# the dump's DDL carries the source's charset and collation.
cubrid createdb newdb en_US.utf8
cubrid server start newdb

# /dumps/mydb must be writable: the manifest and, on a violation, the
# exceptions artifact are written there.
"$IMPORTDB/build/cubrid-importdb" -u dba newdb /dumps/mydb
```

`mydb` and `newdb` are two different databases throughout, and have to be: `-S`
wants the source stopped, the target must be running and empty, and importdb
refuses a target that already holds a user class — naming it. You cannot reload
a database on top of itself.

**"Empty" means no user class, and nothing more than that.** The refusal counts
the user classes in the target's catalog; a database fresh from `createdb` passes,
and one holding a single table of your own does not. The reason is that the
dependency graph is read from the *target's* catalog rather than from the dump, so
a class that was already there would be treated as part of the import — its
PK/UNIQUE/FK dropped before the data phase and rebuilt after, and left dropped if
the run is interrupted in between.

Accounts are **not** part of that count, and do not need to be: the dump carries
the source's users, password hashes and grants in its own DDL, and the definition
phase replays them into the target. So you do not pre-create the users — importing
as `dba` into a fresh `createdb` target is the whole of it, and the accounts arrive
with the schema. Pre-creating an account the dump also defines puts the dump's DDL
in conflict with state it did not expect; leave the target as `createdb` left it.

## Usage

```
importdb: Import an unloaddb dump into a running database.
usage: cubrid-importdb [OPTION] database-name dump-dir

valid options:
    -u, --user=ID               import user; must belong to the DBA group
    -p, --password=PASS         password of the import user
    --degree=N                  inter-table parallel degree
    --continue                  attempt every FK edge instead of stopping at the first rejected one
    --skip-object-classes       skip and report object-valued classes instead of rejecting them
    --dry-run                   print the import plan and terminal tasks; change nothing
    --restart                   ignore an interrupted run's manifest and import from scratch
    --allow-ha                  import into an ha_mode=on target anyway; the standby will NOT
                                receive the rows and must be rebuilt from a backup
    --progress=WHEN             live progress display: auto (default; on when stdout is a
                                terminal), always, or never
    --exceptions-table=NAME     reserved for a future release
    -h, --help                  print this and exit
    --version                   print the CUBRID this binary was built against
```

The target database must exist, be **running**, and be **empty** of user classes.
The dump directory must be **writable** — importdb writes `importdb.manifest`
(the resume point) and, on a violation, `importdb.exceptions` into it.

**`-p` is never prompted for.** importdb has no interactive password path: omit
`-p` and the import user is authenticated with an empty password, which is what a
stock `dba` has and why the examples here pass only `-u dba`. If the account does
have a password, omitting `-p` fails the connection before anything is defined.
Passing one puts it on a command line — importdb's own, and those of the
`cub_admin loaddb` children it forks — where it is briefly visible to anything
that can read `/proc` or run `ps` on the host. That is the same exposure
`cubrid loaddb -p` and `csql -p` already carry, and for the same reason: a forked
loader has no terminal to run the engine's own `getpass` prompt against.

**Exit status is `0` or `1`, and nothing else.** `0` means the run completed with
nothing left over. `1` covers everything else, including cases that are not
failures of the load: a rejected argument, a connection that failed, an aborted
run — but also a run that loaded and committed and then left work behind, whether
that is a withheld FK, an un-rebuilt constraint, a failed statistics update or a
trigger that would not define.

The verdict that tells those apart is **one of two words**, printed in the report
header at the end of a run. Everything else in the output is detail under it:

| final output | exit | what it means |
|---|---|---|
| `===== import report: 'newdb' -- COMPLETE =====` | `0` | The load finished and the catalog was read back and matched the snapshot. Nothing is pending. |
| `===== import report: 'newdb' -- PARTIAL =====` | `1` | The data is **committed and durable**, and something is left over: an un-rebuilt constraint, a withheld FK, a violated edge, a stale statistics update, a failed trigger, or a catalog that did not match. The report names which, and `importdb.manifest` records each with the DDL to finish it. |
| `'newdb' is now PARTIALLY IMPORTED -- ... phase is committed` | `1` | The run was interrupted rather than completed. Everything through the named phase is durable; re-run the same command to continue from there. |
| a refusal line and **no report at all** | `1` | Nothing was defined and nothing was loaded — a rejected argument, a failed connection, a non-empty target, an HA target without `--allow-ha`, an unreadable manifest. The line says which. |

For a script, `COMPLETE` and exit `0` are the pair to gate on; treat `PARTIAL` as
"loaded, now go read the manifest" rather than as a failed load, and the fourth
row as "never started".

Throughout, `mydb` is the database the dump came *from* and `newdb` is the
target being imported *into*. They are never the same database.

```sh
# produce the dump in the first place (server down for -S; -C works with it up)
cubrid unloaddb -S -u dba mydb            # writes mydb_{schema,objects,indexes} here

# the common case
cubrid-importdb -u dba newdb /dumps/mydb

# see the plan without touching anything
cubrid-importdb -u dba --dry-run newdb /dumps/mydb

# parallel: the dump must be per-class for --degree to have anything to spread over
cubrid unloaddb -S -u dba --datafile-per-class mydb
cubrid-importdb -u dba --degree=4 newdb /dumps/mydb

# a dump you suspect: report every violated edge instead of stopping at the first
cubrid-importdb -u dba --continue newdb /dumps/mydb

# resume after a kill -- the same command, nothing new to type
cubrid-importdb -u dba newdb /dumps/mydb

# the SAME dump into a SECOND target -- --restart is required, see below
cubrid-importdb -u dba --restart newdb2 /dumps/mydb

# plain output on a terminal (scripts and pipes get it without asking)
cubrid-importdb -u dba --progress=never newdb /dumps/mydb
```

`unloaddb` writes into the current directory and `-S` needs the server stopped,
so the dump and the import are two separate steps against two separate databases:
you cannot reload a database on top of itself.

**One dump into two targets needs `--restart`, and skipping it fails quietly.**
The manifest lives in the *dump* directory, so it is keyed to the dump and not to
the database it was written for. Importing the same dump a second time — into a
different, empty target, which is an ordinary thing to want, one database for
production and one to verify against — finds the first run's completed manifest
and short-circuits on it:

```
importdb: 'newdb2' is already fully imported from this dump -- the manifest reports
every phase complete; nothing to do (remove /dumps/mydb/importdb.manifest to force
a fresh import).
```

**That is exit `0`, and `newdb2` was never touched.** The check happens before
importdb connects to anything, so the message names the target you asked for
while describing a manifest written for a different one — measured, it prints
this for a database that does not exist at all. Treat it as a defect rather than
a design: a completed manifest ought to be compared against the target the way an
interrupted one already is, which refuses with *"it records a different database
or dump prefix"*.

Until it is, either pass `--restart` (checked before the manifest is consulted,
so it starts the second target cleanly and preserves the prior manifest beside
it), remove `importdb.manifest` between runs, or give each target its own copy of
the dump directory. A script that imports one dump into several databases should
do this unconditionally — the failure is silent and looks like success.

## What it does not do

- **It does not replace `loaddb`.** Single-file, single-table loads are what
  `loaddb` is for, and it is untouched.
- **It does not read foreign dumps.** CUBRID `unloaddb` output only, from 10.2
  or newer. A 9.x dump is not refused by a version check — it will fail wherever
  its DDL or its object data stops parsing, which is the same place `loaddb`
  fails.
- **It is not an online load.** The target must be empty of user classes; this is
  a reload, not an ingest.
- **It does not carry object-valued (OID) columns.** CS-mode loading handles
  value-typed data; classes with object columns are rejected, or skipped and
  reported with `--skip-object-classes`.
- **It does not carry anything `unloaddb` left out of the dump.** What is in the
  dump is replayed, and that is more than it looks: users, their password hashes
  and their grants all survive, verified by logging in as a restored account in
  the `crossversion` case. Anything `unloaddb` does not write — a stored
  procedure, say — importdb cannot invent.
- **It does not replicate to an HA standby.** The bare-heap load produces no row
  replication, so an `ha_mode=on` target is refused unless you pass `--allow-ha`
  and rebuild the standby from a backup afterwards.
- **It cannot tell a truncated object file from a shorter dump.** An `unloaddb`
  dump carries no per-class row count, so nothing downstream — `loaddb` or
  importdb — can tell a file that was cut short from one that legitimately holds
  fewer rows. A truncated object file therefore imports at **exit 0** and reports
  `COMPLETE`: measured, halving one 41-row object file loaded 20 rows and called
  it done. So verify the dump before importing it, and compare row counts against
  the source afterwards — the report's `loaded:` block gives one count per object
  file, which is per class for a `--datafile-per-class` dump and a single total
  for a default one. Closing this needs the count written into the dump, which is
  an `unloaddb` change, not an importdb one. A *deleted* per-class object file is
  a different case and **is** refused, naming the class: the schema states the
  class list, so a missing file is derivable where a short one is not.

## Development

```sh
cmake --build build -j                       # build
tests/run_tests.sh                           # the functional suite
tests/run_tests.sh -k resume fkviolation     # one or more cases, keep the scratch dir
bash demo/run_demo.sh build/cubrid-importdb  # the demo, four scenarios
ctest --test-dir build --output-on-failure   # contract + smoke + functional

# the cross-version lane: a second, OLDER install to write the dump with
tools/fetch_engine.sh --release 10.2_latest --install-only engine102
IT_SRC_CUBRID=$PWD/engine102/install tests/run_tests.sh crossversion
```

`ctest` includes the performance case, which builds a 393,000-row fixture and runs
paired imports at two degrees — budget an hour for the lot, or run
`tests/run_tests.sh` with the case names you want instead. Without
`IT_SRC_CUBRID` the `crossversion` case prints a `SKIP` with that reason; every
other case needs only the one install.

- [`tests/`](tests/README.md) — 14 cases, 285 assertions outside the performance
  case (which adds its own per measured pair): round-trip fidelity, every
  column-type family, dependency ordering, FK cycles, FK violations,
  `--dry-run`, `--degree`, resume after `SIGKILL`, refusals, a damaged dump, the
  pre-11.5 compatibility rewrite, and a 10.2 dump imported into 11.5.
- [`demo/`](demo/README.md) — 4 scenarios, 34 assertions: the `loaddb` contrast,
  the plan, the FK violation, and what parallelism actually depends on.
- [`contract/`](contract/README.md) — what this repo depends on from CUBRID,
  enumerated and machine-checked: 41 static checks against an install, 16 runtime
  checks against a live database, plus a negative control that must fail.
- `docs/` — [`output.md`](docs/output.md) (everything the tool prints),
  [`performance.md`](docs/performance.md) (the method, the numbers, and what
  measured out as wrong), [`old-dumps.md`](docs/old-dumps.md) (the 10.2 floor and
  the shapes that have guards), and
  [`out-of-tree.md`](docs/out-of-tree.md) (how the build works against an engine
  it does not live in, and the libstdc++ ABI question).

## License

Apache License 2.0, following CUBRID. See [LICENSE](LICENSE).
