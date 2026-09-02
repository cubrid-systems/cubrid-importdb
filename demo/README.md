# demo — what `cubrid-importdb` does that a hand-scripted `loaddb` does not

One command, four scenarios, every claim checked against the catalog:

```sh
export CUBRID=/path/to/a/cubrid/install          # an install, not a source tree
bash demo/run_demo.sh path/to/cubrid-importdb    # or set IMPORTDB_BIN
```

The binary argument is optional — with none given the script uses
`$IMPORTDB_BIN`, then `build/cubrid-importdb` next to the repo.

Pass `-k` (or set `KEEP=1`) to leave the scratch directory, its databases and
all the logs behind for inspection; the script prints the path it kept.

It `mktemp -d`s a scratch directory outside the tree, points **both**
`$CUBRID_DATABASES` and its working directory at it, creates eight databases of
its own named `idbdemo_*` inside it, and removes the lot on exit (`trap`).
Nothing outside that directory is read or written, and it only ever stops or
drops a database this run created. Exit status is `0` if every assertion
passed, `1` if one failed, `77` if it could not set up (so it can be registered
as a skippable CTest case).

Runtime is a couple of minutes on a quiet host, and almost all of it is CUBRID
`createdb` plus the server start/stops — the actual loading is seconds.

## The dump this starts from

Type nothing extra and `unloaddb` gives you **three files**, named after the
database:

```
$ cubrid unloaddb -S -u dba idbdemo_src
idbdemo_src_schema        the DDL: classes, columns, PK/UNIQUE, then the FKs
idbdemo_src_objects       ALL the row data, every class, in one file
idbdemo_src_indexes       the plain secondary indexes
idbdemo_src_unloaddb.log
```

That is the dump an operator has in hand, so that is the dump scenarios 1, 2
and 3 reload.

`--datafile-per-class` is a **non-default** option that splits the row data
into one file per class:

```
$ cubrid unloaddb -S -u dba --datafile-per-class idbdemo_src
idbdemo_src_schema
idbdemo_src_indexes
idbdemo_src_dba.region_objects
idbdemo_src_dba.product_objects
idbdemo_src_dba.audit_log_objects
idbdemo_src_dba.customer_objects
idbdemo_src_dba.orders_objects
```

importdb reads both layouts — they are `object_layout::SINGLE` and
`object_layout::PER_CLASS` in `src/import_discovery.cpp`, and it prints which
one it found (`objects: single (…)` / `objects: per-class (5 file(s))`).

The layout matters for exactly one thing, and scenario 4 is about it:
**`--degree` fans the data phase out over object files**, so a default dump has
nothing to fan out over. Stage 0 therefore builds both dumps and keeps them as
read-only fixtures; every scenario runs against its own copy.

## What is in here

| file | what it is |
|---|---|
| `run_demo.sh` | the driver — all four scenarios, with PASS/FAIL assertions |
| `schema.sql` | the schema being dumped and reloaded |
| `gen_rows.sh` | emits the 3008 `INSERT` statements (generated, not checked in) |

## The schema, and why it is shaped this way

```
          region            product          audit_log
          (PK, UNIQUE)      (PK, UNIQUE)     (PK on an AUTO_INCREMENT column)
             |                  |
             | FK               |
             v                  |
          customer              | FK
          (PK, UNIQUE,          |
           plain index)         |
             |                  |
             | FK               |
             v                  v
                   orders
                   (PK, two FKs, plain index)
```

- **Three dependency levels.** `L0 = [audit_log, product, region]`,
  `L1 = [customer]`, `L2 = [orders]`. L0 has three members, so `--degree` has
  something to fan out over *once the dump is split per class*.
- **A PK, a UNIQUE that is not the PK, and a plain secondary index** — the
  three constraint kinds the heap-only lifecycle treats differently (PK and
  UNIQUE are stripped and bulk-rebuilt; plain indexes are deferred and built;
  FKs are re-validated and only then defined).
- **An `AUTO_INCREMENT` column** (`audit_log.entry_id`), so a serial has to
  survive the round-trip. The demo compares `_db_serial` on both sides.
- 3008 rows total. Deliberately small: this is a demo, not a benchmark.

## Scenario 1 — the baseline contrast

The default dump is reloaded twice: once the way an operator does it today,
once with `cubrid-importdb`.

The old way, printed by the script as it runs them:

```
cubrid loaddb -S -u dba -s idbdemo_src_schema   idbdemo_old
cubrid loaddb -S -u dba -d idbdemo_src_objects  idbdemo_old
cubrid loaddb -S -u dba -i idbdemo_src_indexes  idbdemo_old
```

Three invocations, and the operator supplies the order: `-s` before `-d`
before `-i`, one `--data-file` at a time. It is only three because the dump is
the default one; add `--datafile-per-class` and it becomes one `-d` per class,
so a forty-class dump is forty-two invocations in the right sequence.

The new way:

```
cubrid-importdb -u dba idbdemo_new <dump-dir>
```

**What to look for in the output.** importdb narrates its own lifecycle, and
those lines are the point of the scenario:

```
importdb: rostered default dump (prefix 'idbdemo_src') from <dump-dir>
    schema:   idbdemo_src_schema (single)
    objects:  single (idbdemo_src_objects)
    indexes:  idbdemo_src_indexes
importdb: defined 'idbdemo_new'.
importdb: dependency graph for 'idbdemo_new' -- 5 node(s), 3 FK edge(s), 0 inheritance edge(s), 1 serial(s)
importdb: schedule for 'idbdemo_new' -- data phase 3 level(s), 20 terminal task(s)
importdb: stripped 11 constraint(s) from 'idbdemo_new'.
importdb: loaded 3008 row(s) from 1 object file(s) into 'idbdemo_new'; ...
importdb: rebuilt 8 constraint(s) and built 2 index(es) on 'idbdemo_new'; ...
importdb: FK re-validation of 'idbdemo_new' passed -- 3 edge(s) validated clean, ...
importdb: defined 3 FK(s) on 'idbdemo_new'.
importdb: updated statistics on 5 class(es) in 'idbdemo_new'.
```

Read it as one sentence: *roster the dump, define the schema, read the catalog
to learn the dependencies, snapshot and then **strip** every PK/UNIQUE/FK, load
into bare heaps, bulk-rebuild the keys, re-validate the FKs against the data
that actually landed, define the FKs, refresh statistics.* That is the
heap-only lifecycle, and loading into a heap with no index to maintain is where
the speed comes from. Note `from 1 object file(s)` — this is the default dump,
and none of that machinery needed the dump to be split.

**What is asserted.** Not "trust me, it's the same" — the script snapshots
both databases (row counts per table, every `db_index` row joined to its
`db_index_key` columns, every `db_attribute` row including the
`AUTO_INCREMENT` default, and `_db_serial`) and `diff`s them. A difference is
printed as a diff and fails the run.

**About the wall clock.** The script prints both. It is a single run of one
3008-row dump on one host, with the `loaddb` path in standalone mode and
importdb against a live server. It is there so the transcript is honest about
what you just watched. It is **not** a benchmark and must not be quoted as
one.

## Scenario 2 — the plan (`--dry-run`)

`--dry-run` connects, defines the schema, reads the catalog, builds the
dependency graph, computes the schedule, prints it, and rolls the whole thing
back.

What to look for: the level sets and the terminal task list.

```
  data phase (parallel-eligible level sets):
      L0: [audit_log, product, region]
      L1: [customer]
      L2: [orders]
  terminal tasks (in a valid execution order):
      #0  REBUILD_PK      audit_log (pk_audit_log)
      ...
      #9  FK_VALIDATE     customer -> region (fk_customer_region)   [after #4, #7]
      #12 FK_DEFINE       customer -> region (fk_customer_region)   [after #4, #7, #9]
```

Those levels come from the **catalog**, not from the dump layout: this is the
single-file dump, and it plans exactly the same three levels a per-class dump
would. The `[after #N]` edges are the interesting part — every `FK_DEFINE` is
predicated on its own `FK_VALIDATE` and on the rebuild of the parent key it
points at. That ordering is what scenario 3 exercises.

Asserted: exit 0, three levels printed, all three `FK_DEFINE` tasks carry
prerequisites — and then that the target database has **zero** user classes
afterwards and that no `importdb.manifest` was written into the dump
directory. Nothing changed.

## Scenario 3 — the dump with orphan rows (the one that matters)

The script copies the default dump and inserts two rows into the `orders`
block of `idbdemo_src_objects`:

```
900001 900001 1 1
900002 900002 1 2
```

`order_id customer_id product_id qty`. Customers 900001 and 900002 do not
exist. Product 1 does, so exactly one of the three FK edges is violated —
which is also what lets the scenario show that the *other two* FKs still get
defined.

**Where the rows go matters.** The single-file layout concatenates per-class
blocks, each introduced by a `%class [owner].[name] (col…)` header that
declares the column list for the rows that follow:

```
%id [dba].[region] 26
...
%class [dba].[region] ([region_id] [region_code] [region_name])
1 'R01' 'Region 01'
...
%class [dba].[orders] ([order_id] [customer_id] [product_id] [qty])
1 2 2 2
...
```

A row appended to the **end of the file** belongs to whichever class happens
to be last, not to the class you meant. The script inserts immediately after
the `[orders]` header instead, which is correct whatever the block order, and
then asserts it: `blocks_of_rows()` walks the file and reports which `%class`
block each injected row actually fell into, and the assertion demands
`orders orders`. After the load it also asserts all five row counts, so a
misdirected row would show up as `region 10` rather than `orders 2002`.

### (a) the old way accepts it, silently

```
Total 3010 object(s) inserted, 0 object(s) failed.
```

Exit status 0. Nothing on stderr. And then, from the catalog:

| checked | result |
|---|---|
| rows in `orders` | 2002 |
| the other four tables | 8 / 200 / 300 / 500 — exactly as dumped |
| `fk_orders_customer` present in `db_index` | 1 |
| `orders` rows with no matching `customer` | 2 |

Read the last two lines together. The FOREIGN KEY is in the catalog **and**
two rows violate it. `loaddb` turns FK checking off for the data phase — that
is why one `-d` over a file whose `orders` block precedes nothing works at all
— and it never turns it back on to re-check. The reload "succeeded" and the
database is referentially broken.

This is not a hypothetical about a hand-written script getting the order
wrong. It is what `loaddb` does when handed a dump whose data does not satisfy
its own schema, which is exactly the dump you get from a database that was
*already* broken, or from an `unloaddb` of a subset of classes.

### (b) importdb detects it, enumerates it, and withholds the FK

```
importdb: FK re-validation of 'idbdemo_fknew' found violations -- 1 edge(s) violated, 2 orphan row(s) total; ...
importdb: defined 2 FK(s), withheld 1 on 'idbdemo_fknew' ...
  withheld FKs (1) -- defined after the data is repaired:
      orders -> customer (fk_orders_customer) :: FK re-validation found 2 orphan row(s)
        re-add: ALTER CLASS [dba].[orders] ADD CONSTRAINT [fk_orders_customer] ...
```

Exit status 1. The data is still loaded (2002 rows — importdb does not throw
away your import), the two clean FK edges are defined, and the violated one is
**not** in the catalog. Next to the dump it leaves `importdb.exceptions`:

```
[edge] orders -> customer (fk_orders_customer)
child_key_columns: order_id
fk_columns: customer_id
orphans: 2
orphan: child[order_id=900001] fk[customer_id=900001]
orphan: child[order_id=900002] fk[customer_id=900002]
...
readd: ALTER CLASS [dba].[orders] ADD CONSTRAINT [fk_orders_customer] ...
```

Every offending row, named by its own primary key, plus the DDL to put the FK
back once they are fixed.

`--continue` is what makes it enumerate *every* violated edge; the default is
fail-fast on the first one.

### (c) was withholding it the right call? ask the engine

The scenario finishes by taking the `readd:` statement out of the exceptions
file and running it against the importdb target:

```
ERROR: The constraint of the foreign key 'fk_orders_customer' is invalid, due to value '900001'.
```

The engine refuses to create that FOREIGN KEY over that data. So the state the
`loaddb` path was left in — FK present, two rows violating it — is a state the
engine itself would never have allowed through DDL. importdb declining to
create it is not conservatism; it is the only answer consistent with what the
engine enforces everywhere else.

Then the script deletes the two rows the record names, re-runs the recorded
DDL, and asserts the FK is now defined and the row count is back to 2000. The
exceptions file is not a log message — it is a repair procedure.

## Scenario 4 — `--degree=4` twice: the dump layout is what buys parallelism

`--degree=N` fans the data phase out over **object files**, through a bounded
pool of `cub_admin loaddb -C` children, and the degree is clamped to the number
of files (`import_load.cpp`: `if (!files.empty () && (size_t) deg >
files.size ()) deg = files.size ()`). So the same flag does two different
things depending on how the dump was taken. The scenario runs it both ways.

**4a — on the default dump (one object file):**

```
    objects:  single (idbdemo_src_objects)
importdb: loaded 3008 row(s) from 1 object file(s) into 'idbdemo_pardef'; ...
```

Exit 0, and the flag did nothing. One object file means one child, so the
degree collapses to 1 and the data phase prints the plain serial line — no
`at degree` in the output at all. Asserted: exit 0, one object file, and
**zero** `at degree` lines. Nothing warns you about this, which is the reason
it is in the demo rather than in a footnote.

**4b — on the `--datafile-per-class` dump (five object files):**

```
    objects:  per-class (5 file(s))
importdb: loaded 3008 row(s) from 5 object file(s) into 'idbdemo_parpc' at degree 4.
```

Asserted: exit 0, five object files, the data phase reports the degree it
used, and the resulting state `diff`s clean against scenario 1's serial run
off the default dump.

Deliberately **not** asserted: that 4b is faster than 4a. Five object files
and three thousand rows is far below the point where fan-out shows up as time.
The two claims under test are that `--degree` needs
`unloaddb --datafile-per-class` to do anything at all, and that when it does,
the result does not change.

## Things the demo does not show

- **Resume.** A killed import re-runs from its manifest instead of starting
  over (`importdb.manifest` in the dump directory, `--restart` to ignore it).
  Demonstrating it means killing the process at a phase boundary, which is
  timing-dependent and would make the demo flaky, so it is left out. The
  manifest itself is visible in the dump directory after every non-dry run.
- **Inheritance and partitioning edges.** The dependency graph carries them,
  but this schema has neither; the graph line reports `0 inheritance edge(s)`.
- **`--skip-object-classes`.** Needs a class with an object-valued (OID)
  column, which CS-mode load cannot parse. Worth knowing that on a *default*
  dump importdb refuses to skip at all — message 61 says the instances share
  one `<prefix>_objects` file and tells you to re-dump with
  `--datafile-per-class`. Out of scope here.
- **The HA refusal.** importdb refuses to import into an HA target unless
  `--allow-ha` is given, because loading into bare heaps writes no row
  replication records and the standby would silently end up empty. Showing it
  needs an HA pair.

## Notes for whoever maintains this

- `csql -i file.sql` exits **0 even when a statement inside it fails**, so the
  script greps those logs for `^ERROR`. `csql -c '<stmt>'` *does* propagate the
  failure in its exit status, and the script relies on that where it wants a
  statement to be rejected (the `readd` probe in scenario 3).
- `cubrid loaddb -S` and `cubrid unloaddb -S` need the server **down**;
  `cubrid-importdb` needs it **up** (it is a CS-only utility). The script
  brackets each step with `cubrid server start`/`stop` accordingly.
- All verification goes through `csql -C` with the server up, not `csql -S`.
  A standalone `csql` pays a full server bootstrap and a standalone vacuum pass
  per invocation (about three seconds here), and the state snapshot alone makes
  eight queries per database. Moving the reads to client-server took the whole
  demo from about six minutes to about one and a half.
- Each scenario gets its **own copy** of the dump directory, because importdb
  writes `importdb.manifest` (and, on a violation, `importdb.exceptions`)
  next to the dump — and a leftover manifest is exactly what triggers the
  resume path on the next run. `$W/dump-default` and `$W/dump-per-class` stay
  pristine.
- An `AUTO_INCREMENT` column's serial does not appear in the `db_serial` view;
  it is in `_db_serial`. That is what the state snapshot reads.
- `$CUBRID_DATABASES` is **not** enough on its own to keep CUBRID out of the
  source tree. It is where `databases.txt` lives and where a database is looked
  up by name, but the volume files are created relative to the **working
  directory**, and `csql`/`loaddb` also drop `csql.err`, `csql.access`, `lob/`
  and their own `*_loaddb.log` there. The script therefore `cd`s into its
  scratch directory as well. A 128 MB volume file committed by accident is a
  push GitHub refuses outright, so do not remove either half of that.
- The data phase runs `cub_admin loaddb -C` children at every degree, serial
  being degree 1. The wording the demo greps for is
  `loaded N row(s) from M object file(s) into '<db>'` at degree 1, and the same
  line with `at degree N (inter-table parallel` appended when the fan-out is
  real.
