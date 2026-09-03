# tests/ — the functional suite

`tools/smoke.sh` answers one question: does this build import at all? It is
deliberately thin — one two-table dump, six catalog assertions — because it runs
on every nightly and has to stay cheap.

This directory answers the rest. Each case builds its own source database from a
fixture, unloads it with the installed `unloaddb`, imports it with the binary
under test, and asserts on the result — the catalog, the rows, the manifest, the
exceptions artifact, and the tool's own printed output. Nothing is shared
between cases except the helpers.

## Running it

```sh
export CUBRID=/path/to/cubrid            # an install, not a source tree
tests/run_tests.sh --bin=build/cubrid-importdb          # everything
tests/run_tests.sh --bin=build/cubrid-importdb roundtrip fkcycle
tests/run_tests.sh -k resume             # keep the scratch dir to poke at it
tests/run_tests.sh -l                    # list the cases
```

`--bin` may be replaced by `IMPORTDB_BIN=/path/...` in the environment; with
neither, the runner looks for `build/cubrid-importdb` and then for
`cubrid-importdb` on `$PATH`.

One case needs a **second, older** CUBRID install, because it is the only one
that can see a cross-version break at all — every other case unloads and
imports with the same install:

```sh
tools/fetch_engine.sh --release 10.2_latest --install-only engine102
IT_SRC_CUBRID=$PWD/engine102/install tests/run_tests.sh crossversion
```

Without `IT_SRC_CUBRID` the `crossversion` case prints a `SKIP` with that
reason and the suite stays green. The old engine is used **standalone only** —
`createdb`, `csql -S`, `unloaddb -S` — so the two installs need no port
separation, no second `cub_master`, and never see each other. That is why the
lane costs one environment variable instead of a second harness.

Through CTest, which runs every case including `perf`:

```sh
ctest --test-dir build -R functional --output-on-failure
```

CI calls the runner directly instead, with `perf` left off the list: its verdict
is a ratio measured on the host it ran on, and a shared runner is the worst place
both to measure that and to trust the number.

Exit codes are the same contract `tools/smoke.sh` uses: **0** everything passed,
**1** something failed, **77** the environment cannot run the suite at all — no
`$CUBRID`, no binary, or a database cannot be created and served here. CTest
reads 77 as SKIP, which is why an unconfigured host reports a skip rather than a
red build.

### Where it puts things

Every database and every dump lives under a scratch directory created per run
with `mktemp -d "${TMPDIR:-/tmp}/importdb-tests.XXXXXX"`, **never inside the
repo** — a CUBRID volume file is 64 MB or more and GitHub refuses files over
100 MB. Each case gets its own subdirectory and its own `CUBRID_DATABASES`, and
`cd`s into it before touching `cubrid`. On exit a trap stops and deletes only
the databases that case created, then removes the directory; `-k` keeps the
directory and prints the path.

Databases are named `it_<case>_<role>`. Nothing the suite did not create is ever
stopped or deleted.

A cross-version case adds a second `CUBRID_DATABASES` under the same case
directory for the old engine's databases, and its own directory of library
symlinks for the sonames that engine still wants. Those databases are dropped
with the engine that made them — the current engine would refuse the volume
format — and everything else the case directory holds goes with the `rm -rf`.

## What each case covers

| case | what it proves |
|---|---|
| `types` | Every column-type family survives the round trip. One table per family, so a failure names the family: ENUM, JSON, BLOB/CLOB, BIT, SET/MULTISET/SEQUENCE, the four zoned date/time types, MONETARY, NCHAR, two collations in one row, non-ASCII text, HASH and LIST partitioning, a filtered index, a function index, and one VARCHAR value larger than `unloaddb`'s internal buffer — the shape that came back corrupted in CBRD-26282. Compared exactly as `roundtrip` compares: byte-identical catalog fingerprint, per-class row counts and content checksums. Then 25 of the families are asserted by name, so a diff-only failure cannot hide which one was lost. |
| `roundtrip` | The import reproduces the source. A fixture carrying a PK, a single-column UNIQUE, NOT NULL, DEFAULT, an FK, a plain index, a composite index with a DESC key, a multi-column UNIQUE index, a REVERSE index, a keyless table, an advanced serial and a trigger is compared against its import two ways: a tagged catalog fingerprint (`db_class`, `db_attribute`, `db_index`, `db_index_key`, `db_direct_super_class`, `db_partition`, `db_serial`, `db_trigger`) that must be byte-identical, and per-class row counts plus order-independent content checksums (rows sorted, then hashed — the physical load order differs). The counts are checked twice, once derived from the rows that were hashed and once asked of the server with `count(*)`, so a checksum that agrees for the wrong reason still fails. Also: the trigger is defined last, so it must not have fired on the bulk-loaded rows. |
| `ordering` | A schema with three levels of FK edges below the root, one inheritance edge and a RANGE-partitioned table imports cleanly, and the level sets the tool *prints* are topologically valid — every parent strictly below its child, every superclass strictly below its subclass. The check parses the printed graph and the printed plan, which is what an operator reads. |
| `fkcycle` | Two tables that reference each other import in one command. importdb strips the constraints before the data phase and defines them again afterwards, so both FKs are present at the end and no FK is withheld. |
| `dupname` | The same constraint name on more than one class &mdash; three classes each with a PRIMARY KEY named `pk1`, two of them also with a UNIQUE named `u1`, which CUBRID allows because index names are unique per class rather than per database. Every one of them must be back at the end (catalog fingerprint) **and** recorded against the class it belongs to (the manifest's `[rebuild]` section, which is what a repair is driven from). The dump is the DEFAULT single-file layout on purpose: its name filter is what has to resolve the repeated names. |
| `fkviolation` | A dump with orphan rows is detected, the offending rows are enumerated into `importdb.exceptions`, the run exits non-zero, and the violated FK is **withheld** — verified absent from `db_index`, with its re-add DDL in the manifest `[fkdefine]` section. The recorded DDL is then executed against the repaired data to prove it is the real repair. Default policy stops at the first violated edge; `--continue` reports both edges and all three orphans. |
| `dryrun` | `--dry-run` changes nothing. Both halves are asserted: the plan really was produced (the graph is built from a real catalog read of the target, so the schema really was defined), and the catalog before and after is identical, with no manifest and no exceptions file written. Then a real import into the same database succeeds and round-trips — which it cannot if the rollback left anything behind. |
| `degree` | `--degree=1`, `2` and `4` on a `--datafile-per-class` dump with five object files reach the same final state, and the same state as the source. Note that the data phase spawns `cub_admin loaddb -C` children through a bounded pool at *every* degree, so this is testing the pool's bound, not two different implementations. |
| `resume` | `SIGKILL` mid-import, then the same command again. Two kill points, because they are different paths: **part-way into a phase** (wait for the manifest to record `stripped`, then kill while rows are loading — the resumed run must empty the tables and reload) and **at a phase boundary** (wait for `loaded`, which is written only after the data phase committed, then kill — the resumed run skips the data phase and re-enters the rebuild under its guard). Both must land on exactly the state an uninterrupted import produces, compared against a baseline import of the same dump. A third identical run must be a no-op. |
| `refusals` | The things that must be refusals rather than surprises: no arguments, a missing positional, a missing dump directory, a directory with no dump in it, a dump with a schema file but no object roster, `--exceptions-table` (reserved), a non-DBA user, and a non-empty target &mdash; both a class whose name collides with the dump's and one the dump does not define at all. Each asserts the non-zero exit *and* the specific diagnostic, so a refusal that starts happening for a different reason still fails. |
| `corrupt` | A damaged dump, six ways, each from a pristine copy of one good dump. **Caught:** a value of the wrong data type (the object file is named and the loader's own failed-object count is surfaced; nothing is committed), an object file of random bytes, a schema file that will not parse (file *and* line named, and the all-or-nothing definition leaves the target with zero classes and no manifest), and a manifest that records no phase (refused, and the file is left byte-identical rather than overwritten). **Not caught, and asserted as such:** a truncated object file loads what it can and reports `COMPLETE` at exit 0, and a deleted per-class object file leaves that class silently empty at exit 0. See the note below — the cause is upstream and the same for both. |
| `compat` | The pre-11.5 `CALL ... ON CLASS` rewrite, and what it must not touch. Its dump is hand-written and checked in under `fixtures/dumps/`, so this guard runs on **every** host — unlike `crossversion`, which needs a real old install. The fixture puts every string the scanner looks for somewhere it must not act: inside a `DEFAULT`'s string literal, after a `--` marker *inside* that literal, and inside a loaded row. It also carries the two exempt `find_user` statements and the one `change_serial_owner` that is not exempt, so the reported rewrite count — **exactly 1** — is the assertion: 3 would mean the exemption was lost, more would mean the scanner is matching inside literals. Both literals are then read back verbatim, and the schema file on disk is checked unmodified. |
| `crossversion` | A dump written by an **older** engine imports correctly. The `types` and `legacy` fixtures are built on the old engine, unloaded with *its* `unloaddb`, and imported by the binary under test; a reference arm builds the same two fixtures on the current engine and imports its dump the same way. Three comparisons, none of them loosened for being cross-version: the **data** (old source vs target, per-class counts and content checksums, byte-identical), the **catalog** (target-from-old-dump vs target-from-current-dump, through the same strict fingerprint `roundtrip` uses, byte-identical), and the **auth** set (users and their grants on the dump's own classes, byte-identical). The reference arm is what makes the catalog half possible at all: both sides are current-version catalogs, so no version-neutral column subset has to be invented. It also pins the two shapes that make the dump old — `on class [db_serial]` and `call [change_owner]` — and asserts the compat rewrite fired **exactly once**, since the same dump carries two `find_user ... on class [db_user]` statements that must *not* be rewritten. |

### What is not covered here, and why

**`ha_mode=on` target.** The `refusals` case prints a `SKIP` with the reason
rather than faking it. Asserting the HA guard needs a real HA master with a
standby — a second host or a docker pair — which this harness cannot provision.
Forcing `ha_mode` on a lone server would assert against a fixture that is not
HA. The guard itself is in `src/import_db.cpp` (`HA_DISABLED ()` /
`--allow-ha`), and the `m5 ha51b-docker` fixture referenced from
`src/import_define.cpp` is where it was measured.

**A damaged dump that cannot be detected.** `corrupt` asserts, rather than
wishes away, that a **truncated** object file and a **deleted** per-class object
file both import at exit 0 and report `COMPLETE`. One cause covers both: an
`unloaddb` dump carries no statement of what it contains — no per-class row
count, no roster of the object files that should exist — so a dump missing
content is indistinguishable from a dump *of* less content, to `loaddb` and to
importdb alike. Closing it needs the count in the dump, which is an `unloaddb`
change. Measured: halving one 41-row object file imported 20 rows and called it
complete. The case asserts the short count, so if upstream ever does add a
check, this test fails and someone updates it deliberately.

**Cross-version coverage stops at 10.2, deliberately.** The two hard 9.x → 10.x
breaks are below that floor and are not a loader's to fix: default password
hashing changed from SHA1 to SHA2 in 10.0 (CBRD-20659) and the `reuse_oid`
default changed in 10.0 (CBRD-23708). Both need a migration step before any
loader runs, so a lane that started at 9.3 would be testing the migration step,
not the import.

**Three upstream properties the cases pin down rather than test.** All three are
`unloaddb`/engine behaviour, not importdb's, and each was found by this suite:

* `unloaddb` writes a serial's **current** value as its `START WITH`, so an
  imported serial's `db_serial.start_val` equals the source's `current_val`.
  `start_val` is therefore excluded from the catalog fingerprint, and
  `roundtrip` asserts the actual behaviour (including the `START WITH` text in
  the dump) instead of pretending it round-trips.
* `cubrid loaddb -C` loads orphan rows into a table whose foreign key is already
  defined, and exits 0 — the constraint ends up present but unsatisfied, with no
  error anywhere. That is the gap `fkviolation` shows importdb closing: the
  set-based anti-join before the FK is defined.
* An `AUTO_INCREMENT` serial's `current_val` is not comparable between a
  standalone reader and a client-server one — the cached allocation differs — and
  the `db_serial` **view** omits AI serials entirely, which is why importdb reads
  `_db_serial` at all. `types` therefore asserts the invariant an operator
  actually depends on, not an equality: the serial's current value is at or above
  the largest id in the loaded data, so it cannot hand out one that already
  exists.

## Layout

```
tests/
├── run_tests.sh            entry point: selection, tally, exit code, preflight
├── lib/
│   ├── common.sh            helpers: databases, SQL, fingerprints, assertions
│   └── fingerprint.sql      the catalog fingerprint query
├── fixtures/
│   ├── roundtrip.sql        every constraint / index / type family
│   ├── types.sql            every column-type family, one table each
│   ├── legacy.sql           what an UPGRADE carries: a named serial, users,
│   │                        grants, an FK, a view, a trigger
│   ├── ordering.sql         FK chain + inheritance + partitions
│   ├── fkcycle.sql          two mutually-referencing tables
│   ├── dupname.sql          one constraint name shared by several classes
│   ├── fkviolation.sql      parent + two children (orphans injected into the dump)
│   ├── wide.sql             parent + four children, for degree and resume
│   ├── dumps/               a hand-written pre-11.5 dump, for the compat guard
│   └── gen_rows.sh          row data, generated rather than committed
└── cases/<name>.sh          one file per case
```

Fixtures hold schemas only. Row data is generated by `gen_rows.sh` so the repo
does not carry megabytes of INSERT statements; the `wide` fixture takes a
row-count argument, which is how `resume` makes the data phase long enough to be
interrupted while `degree` stays cheap.

## The assertion vocabulary

Eight helpers, and nothing else, so the output reads the same everywhere:

```
assert_eq NAME GOT WANT           assert_rc NAME GOT WANT
assert_nonzero_rc NAME GOT        assert_grep NAME FILE REGEX
assert_no_grep NAME FILE REGEX    assert_same NAME EXPECTED ACTUAL
assert_file NAME PATH             assert_no_file NAME PATH
```

Each prints exactly one line, `PASS  <case>  <name>` or `FAIL  <case>  <name>:
<what went wrong>`. `assert_same` prints the first few lines of the diff under
the failure. `note` prints an unnumbered informational line; `skip` prints a
`SKIP` line, which the tally counts separately and which never fails the run.

## What a failure looks like

A failing assertion, and the tally at the end:

```
PASS  roundtrip    import exits clean (exit 0)
FAIL  roundtrip    catalog fingerprint is identical: .../src.catalog and .../tgt.catalog differ
        --- .../src.catalog
        +++ .../tgt.catalog
        @@ -20,7 +20,6 @@
         #IDX  rt_emp  pk_rt_emp_emp_id  YES  NO  1  YES  NO  ...
        -#IDX  rt_emp  ri_emp_salary     NO   YES 1  NO   NO  ...
         #IDX  rt_emp  u_emp_id_nm       YES  NO  2  NO   NO  ...
PASS  roundtrip    row count per class is identical

---- tally ----
assertions : 139 passed, 1 failed, 1 skipped
cases      : 8 run, 1 with failures, 0 aborted
functional: FAIL
  FAIL  roundtrip    catalog fingerprint is identical: ...
```

A green run ends with the tally alone:

```
---- tally ----
assertions : 139 passed, 0 failed, 1 skipped
cases      : 8 run, 0 with failures, 0 aborted
functional: PASS
```

A case that dies before reporting anything — a `die` on a fixture that will not
build, a crash — is counted once as an aborted case, so it cannot pass by
staying quiet:

```
FAIL  degree       case exited 1 without reporting a failure
```

Re-run just that case with `-k` to keep its scratch directory, then look at the
logs it left there: `import.log` (or `d2.log`, `ff.log`, …) is the tool's full
output, `*.catalog` and `*.data` are the fingerprints that were compared, and
the dump directory still holds `importdb.manifest` and `importdb.exceptions`.
