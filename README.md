# cubrid-importdb

**Reload a whole CUBRID database from an `unloaddb` dump with one command — faster
than the hand-scripted way, and without silently leaving a broken foreign key
behind.**

```
cubrid-importdb -u dba mydb /path/to/dump
```

That is the entire operator interface for a full-database reload: point it at the
directory `unloaddb` wrote, and it works out the rest.

---

## The problem

`unloaddb` hands you a directory. Reloading it is on you:

```sh
cubrid loaddb -S -u dba -s mydb_schema   mydb    # schema first, so every
cubrid loaddb -C -u dba -d mydb_objects  mydb    # PK/UNIQUE/FK is live and
cubrid loaddb -S -u dba -i mydb_indexes  mydb    # maintained per row
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

`cubrid-importdb` replaces that with a single command that plans the load from the
dependency graph, loads into bare heaps and bulk-builds the constraints
afterwards, and refuses to define a foreign key the data does not satisfy —
telling you exactly which rows are at fault.

## Features

| | |
|---|---|
| **One command for the whole dump** | Discovers the schema, object and index files, and the layout they were written in — default single-file or `--datafile-per-class`. No ordering for you to get right. |
| **Dependency-aware planning** | Reads the catalog after defining the schema and builds a graph from FK, inheritance, partitioning and serials, then loads in level order. Cycles are handled: FK definition is deferred, so mutually-referencing tables load in one pass. |
| **Heap-only load** | Strips PK/UNIQUE/FK, loads into bare heaps, then bulk-builds the constraints on the populated tables. No per-row index maintenance during the data phase. |
| **Referential integrity is enforced, not assumed** | Every foreign key is defined against the loaded data. If the data violates one, the engine rejects it, and importdb enumerates *every* offending row (the engine names only the first) and withholds that FK rather than defining a broken one. |
| **A repair record you can act on** | `importdb.exceptions` lists each orphan by primary key, and carries the exact DDL to add the withheld FK once the data is fixed. |
| **Inter-table parallelism** | `--degree=N` loads independent tables concurrently. |
| **Resume** | A killed import re-run with the same command continues from its manifest instead of starting over. |
| **Honest about HA** | Refuses an `ha_mode=on` target by default, because the bare-heap load does not replicate to the standby. `--allow-ha` overrides, loudly, twice. |
| **`--dry-run`** | Prints the plan and the terminal task order; changes nothing. |

## Requirements

- **CUBRID 11.5 or newer.** 11.4 and earlier are not supported: importdb reads
  the `_db_serial` system class to find AUTO_INCREMENT serials (the `db_serial`
  view omits them), and that read is rejected on 11.4. The contract check
  (below) reports this rather than letting it fail later and less clearly.
- Linux, a C++17 compiler, CMake 3.16+.
- A CUBRID **source tree** and a configured **build tree** to compile against, plus
  an **installed** CUBRID to link and run against. See [Install](#install).

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

build/cubrid-importdb            # prints the usage
```

Configuring the engine and building only its header producers takes about 20
seconds. The engine itself is never built.

Against a CUBRID you already have, point `CUBRID_SOURCE_DIR` and `CUBRID_BUILD_DIR`
at your own trees and `$CUBRID` at the install.

> The build reads the engine library's libstdc++ ABI with `nm` and matches it.
> Published CUBRID binaries use the pre-C++11 `std::string` ABI; a modern compiler
> does not. You should never have to think about this, but if a link ever fails on
> `cubload::` symbols, `-DFORCE_OLD_CXX_ABI=ON` is the override.

## Usage

```
usage: cubrid-importdb [OPTION] database-name dump-dir

    -u, --user=ID               import user; must belong to the DBA group
    -p, --password=PASS         password of the import user
    --degree=N                  inter-table parallel degree
    --continue                  attempt every FK edge instead of stopping at the first rejected one
    --skip-object-classes       skip and report object-valued classes instead of rejecting them
    --dry-run                   print the import plan and terminal tasks; change nothing
    --restart                   ignore an interrupted run's manifest and import from scratch
    --allow-ha                  import into an ha_mode=on target anyway; the standby will NOT
                                receive the rows and must be rebuilt from a backup
```

The target database must exist, be **running**, and be **empty** of user classes.
The dump directory must be **writable** — importdb writes `importdb.manifest`
(the resume point) and, on a violation, `importdb.exceptions` into it.

**Exit status**: `0` complete · `1` partial (loaded and committed, but at least
one FK withheld — read `importdb.exceptions`) · non-zero otherwise.

```sh
# the common case
cubrid-importdb -u dba mydb /dumps/mydb

# see the plan without touching anything
cubrid-importdb -u dba --dry-run mydb /dumps/mydb

# parallel: needs a --datafile-per-class dump to have anything to fan out over
cubrid unloaddb -S -u dba --datafile-per-class mydb
cubrid-importdb -u dba --degree=4 mydb /dumps/mydb

# a dump you suspect: report every violated edge instead of stopping at the first
cubrid-importdb -u dba --continue mydb /dumps/mydb

# resume after a kill -- the same command, nothing new to type
cubrid-importdb -u dba mydb /dumps/mydb
```

## How it works

Ten phases, each announcing what it did. A real run:

```
importdb: rostered default dump (prefix 'shopsrc') from /dumps/shop
    schema:   shopsrc_schema (single)
    objects:  single (shopsrc_objects)
    indexes:  shopsrc_indexes
importdb: defined 'shop'; the target database is now fully defined and empty.
importdb: dependency graph for 'shop' -- 3 node(s), 2 FK edge(s), 0 inheritance edge(s), 1 serial(s)
  FK edges (child -> parent):
      customer -> region   (fk_cust_region)
      orders -> customer   (fk_ord_cust)
  cycles: none
  level sets (parallel-eligible; complete=true):
      L0: [region]     L1: [customer]     L2: [orders]
importdb: schedule for 'shop' -- data phase 3 level(s), 11 terminal task(s)
      #0  REBUILD_PK      customer (pk_customer_id)
      ...
      #6  FK_DEFINE       customer -> region (fk_cust_region)   [after #2, #4]
      #8  STATS           customer   [after #0, #3, #5]
importdb: stripped 7 constraint(s) from 'shop'; the target now holds bare heaps ready for the data phase.
importdb: loaded 9 row(s) from 1 object file(s) into 'shop'; the target now holds data in bare heaps.
importdb: rebuilt 5 constraint(s) and built 1 index(es) on 'shop'.
importdb: every FOREIGN KEY on 'shop' was accepted by the engine -- 2 edge(s) clean, 0 skipped.
importdb: defined 2 FK(s) on 'shop'; the catalog now matches the dump snapshot (full round-trip complete).
```

**Discover → Define → Graph → Plan → Strip → Load → Rebuild → FK define → Stats →
Triggers.** The shape that matters is the middle: the schema is defined, its
constraint set is snapshotted and then *stripped*, the data goes into bare heaps,
and the constraints are bulk-built afterwards. Triggers are defined strictly last,
so nothing fires during the load.

Foreign keys are defined against the loaded data, which is where the engine
validates them — `ADD CONSTRAINT ... FOREIGN KEY` builds the FK's b-tree over the
existing rows and checks each key against the parent as it goes. importdb does not
duplicate that work; it only takes over where the engine stops.

## Referential integrity

This is the difference worth showing. Take a dump with two orphan `orders` rows
whose `customer_id` does not exist.

**The hand-scripted `loaddb` path** accepts it, exit 0, nothing reported:

```
$ csql -u dba -C -c "SELECT count(*) FROM db_index WHERE index_name='fk_ord_cust'" shop
1                                    <- the FOREIGN KEY is in the catalog
$ csql -u dba -C -c "SELECT count(*) FROM orders o
      WHERE NOT EXISTS (SELECT 1 FROM customer c WHERE c.id = o.customer_id)" shop
2                                    <- and two rows violate it
```

Ask the engine to create that same FK over that data and it refuses. `loaddb` left
a state the engine would not have allowed.

**importdb** on the same dump exits 1, loads the data, defines the clean FKs, and
withholds the violated one:

```
importdb: the engine rejected the FOREIGN KEY and found 2 orphan row(s) on
          'orders' -> 'customer' [fk_ord_cust]; that FK is withheld and its
          offenders enumerated.
importdb: 'shop' has referential violations -- 1 FOREIGN KEY(s) rejected by the
          engine, 2 orphan row(s) total; offenders written to importdb.exceptions.
```

```
# importdb.exceptions
[edge] orders -> customer (fk_ord_cust)
orphan: child[id=900001] fk[customer_id=777]
orphan: child[id=900002] fk[customer_id=888]

# CUBRID importdb FK define: the following FK(s) were NOT defined (withheld).
[fk-withheld] orders -> customer (fk_ord_cust) :: ADD FOREIGN KEY rejected by the engine: 2 orphan row(s)
readd: ALTER TABLE [orders] ADD CONSTRAINT [fk_ord_cust] FOREIGN KEY (customer_id) REFERENCES [customer] (id);
```

The engine names only the first offending value and stops; importdb enumerates all
of them. Fix the data, run the `readd:` statement, and the constraint set matches
the dump. `--continue` attempts every edge instead of stopping at the first.

Run [`demo/run_demo.sh`](demo/README.md) to watch both paths side by side.

## Performance

Measured against a **parallelism-matched** `loaddb` baseline — the baseline runs
its per-class loads concurrently at the same degree, so the comparison isolates
the constraint lifecycle instead of crediting it with the fan-out.

| degree | importdb vs loaddb | what it measures |
|---|---|---|
| **1** | **1.8 – 2.1×** faster | the constraint lifecycle alone |
| **4** | **2.5 – 2.9×** faster | lifecycle plus inter-table parallelism |

393,000 rows over 5 object files, paired runs with alternating arm order, median
of per-pair ratios, four independent runs. A single-host trend, not a certified
benchmark — reproduce it with `tests/run_tests.sh perf`.

Two honest caveats:

- **Small dumps do not benefit.** At ~15,000 rows importdb is *slower* — its fixed
  setup (catalog snapshot, strip, rebuild) dominates. The crossover on this
  hardware is around a hundred thousand rows.
- **`--degree` needs a `--datafile-per-class` dump.** The fan-out is over object
  files, so a default single-file dump clamps to serial no matter what you pass.

## What it does not do

- **It does not replace `loaddb`.** Single-file, single-table loads are what
  `loaddb` is for, and it is untouched.
- **It does not read foreign dumps.** CUBRID `unloaddb` output only.
- **It is not an online load.** The target must be empty of user classes; this is
  a reload, not an ingest.
- **It does not carry object-valued (OID) columns.** CS-mode loading handles
  value-typed data; classes with object columns are rejected, or skipped and
  reported with `--skip-object-classes`.
- **It does not replicate to an HA standby.** The bare-heap load produces no row
  replication, so an `ha_mode=on` target is refused unless you pass `--allow-ha`
  and rebuild the standby from a backup afterwards.

## Development

```sh
cmake --build build -j                       # build
tests/run_tests.sh                           # the functional suite
tests/run_tests.sh -k resume fkviolation     # one or more cases, keep the scratch dir
ctest --test-dir build --output-on-failure   # contract + smoke + functional
bash demo/run_demo.sh build/cubrid-importdb  # the demo, four scenarios
```

| | |
|---|---|
| [`tests/`](tests/README.md) | 9 cases, 202 assertions: round-trip fidelity, dependency ordering, FK cycles, FK violations, `--dry-run`, `--degree`, resume after `SIGKILL`, refusals, and the performance comparison |
| [`demo/`](demo/README.md) | 4 scenarios, 34 assertions — the `loaddb` contrast, the plan, the FK violation, and what parallelism actually depends on |
| [`contract/`](contract/README.md) | What this repo depends on from CUBRID, enumerated and machine-checked: 41 static checks against an install, 16 runtime checks against a live database, plus a negative control that must fail |
| [`docs/out-of-tree.md`](docs/out-of-tree.md) | How the build works against an engine it does not live in, and the libstdc++ ABI question |

CI runs the contract against a nightly engine, builds and imports against that
same build, and — from the engine side — compiles this repo on every upstream PR
that touches a path it depends on. See [`.github/workflows/`](.github/workflows/).

## Provenance

`cubrid-importdb` began as `cubrid importdb` inside the CUBRID engine tree, under
the CUBRID Systems Research roadmap project **N54**. It uses no private engine
API beyond the client headers CUBRID installs, changes no engine code, and drives
the existing `loaddb` loader for the data phase — the whole utility is
orchestration over primitives CUBRID already had.

Apache License 2.0, following CUBRID.
