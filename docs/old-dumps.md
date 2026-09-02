# Reading an older engine's dump

What importdb does about the version gap, and what it cannot do about it. The
README says which dumps it reads; this says why, and what breaks when it does
not.

## The floor and the reason for it

A reload is usually a *migration*: the dump is written by the version you are
leaving and read by the version you are arriving at. importdb reads dumps from
**CUBRID 10.2 and newer**, and the reason it can is mostly structural — the
graph, the plan, the strip, the rebuild and the FK definition are all driven by
a **catalog read of the target** (`src/import_graph.cpp`), not by parsing the
dump's DDL. Whatever version wrote the schema file, once the engine has executed
it the constraint set importdb works from is the current engine's own catalog.

Two things are not structural, and one of them needed code:

**The dump's DDL still has to parse.** It is fed to the engine's own parser, so
importdb inherits exactly `loaddb -s`'s tolerance for old syntax — no more, no
less. One place that was not enough: a pre-11.5 `unloaddb` writes

```
call [change_serial_owner] ('lg_seq', 'DBA') on class [db_serial];
```

and in 11.5 `db_serial` became a *view*, distinct from the `_db_serial` class
that carries the method. `loaddb` rewrites the target in the parse tree
(`ldr_compat_call_target`); the parse tree is not on the installed surface, so
importdb rewrites the same three targets — `db_user`, `db_serial`,
`db_authorization` — in the schema buffer before it is parsed, and prints the
count. `find_user` and `login` are left alone, because the `db_user` view kept
those. Every 10.2 dump of a database that owns a serial carries that statement,
so without the rewrite the whole class of dumps was refused at the first serial.

**The object data is `loaddb`'s to parse.** The data phase execs
`cub_admin loaddb -C` per object file, so whatever that build accepts, importdb
accepts. This is not a layer that can fix a `loaddb` bug, and it does not claim
to.

**Why 10.2 and not older.** The two hard 9.x → 10.x breaks are below that floor
and are not a loader's to fix: default password hashing changed from SHA1 to SHA2
in 10.0 (CBRD-20659), and the `reuse_oid` default changed in 10.0 (CBRD-23708).
Both want a migration step before any loader runs.

`tests/run_tests.sh crossversion` is the evidence, and it is not a smoke test.
It builds the fixtures on a real 10.2 install, unloads them with *that* engine's
`unloaddb`, imports the result, and then makes three byte-identical comparisons —
the data against the 10.2 source, and the catalog and the grant set against a
target built from a *current* dump of the same fixtures. Measured on 10.2.18
against 11.5.0.2498: 16 classes, 91 rows, one rewritten target, identical on all
three.


## What is guarded, and how

Three shapes of old dump have their own guard in the suite, because each of them
is a failure that happened to somebody:

| guard | shape | outcome |
|---|---|---|
| `compat` | `CALL ... ON CLASS db_serial` | rewritten, and the count is reported |
| `compat` | a `CALL` target importdb does not rewrite | the target is named, so the failure is not an arbitrary parse error |
| `compat` | a synonym colliding with its own class | named as itself rather than read as a non-empty target |
| `crossversion` | a view whose query spec unloaddb wrote as `NA` | imports, and both specs come back |

`compat`'s dumps are hand-written and checked in, so those three run on every
host. `crossversion` needs a real old install and skips without one.

## Two shapes that cannot be guarded here

**A synonym colliding with its own class is not importdb's to fix.** 11.2 writes
synonyms before classes so a view may use one, and an `unloaddb` before 11.2
Patch 7 wrote every class unqualified and moved the owner afterwards. Together
they ask for a DBA-owned class that the synonym already is:

```
importdb: definition failed in .../syn112_schema at line 8: ... Class dba.sy_t already exists.
loaddb:   ERROR: Class dba.sy_t already exists.
```

Both loaders fail identically. The statements are in the wrong order inside the
dump; CBRD-24974 fixed the class half in 11.2 Patch 7, and re-dumping with a
patched engine is the repair. What importdb adds is only that the diagnosis says
which failure this is, because those words are also what the engine says about a
target that is not empty.

**9.x is below the floor.** ftp.cubrid.org publishes no 9.3 Linux install, only
source, so a 9.x lane cannot be built the way the 10.2 one was. It would be
testing the migration step rather than the import in any case.
