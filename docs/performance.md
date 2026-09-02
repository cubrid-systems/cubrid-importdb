# Performance

Two measurements live here. One compares importdb against the thing it replaces,
at the size the test suite runs. The other asks where the time goes when the data
is ten times larger, and answers a different question than expected.

## How the comparison is made

Against a **parallelism-matched** `loaddb` baseline: the baseline runs its
per-class loads concurrently through a pool with the same bound, over the same
object files, so the comparison isolates the constraint lifecycle instead of
crediting importdb with the fan-out. Both arms run every phase in CS mode against
the same running server, and both update statistics.

`tests/run_tests.sh perf` **is** the measurement, and the only source for the
table below — there is no separate benchmark. It builds a 393,000-row fixture over
five object files, unloads it per class, and runs the two arms back to back in
pairs: one unmeasured warmup pair, then alternating arm order, reporting the
**median of the per-pair ratios**, because drift that moves both arms of a pair
barely moves their ratio.

| degree | loaddb | importdb | median ratio | speedup |
|---|---|---|---|---|
| **1** | 5.70 s | 2.68 s | 0.481 &nbsp;(pairs 0.444 – 0.677) | **2.13× faster** |
| **4** | 5.08 s | 1.67 s | 0.362 &nbsp;(pairs 0.327 – 0.410) | **3.04× faster** |

Medians of five counted pairs each, 2026-08-28, on a 16-core Linux host at load
0.68 against CUBRID 11.5.0.2494 — the run itself is kept at
[`perf-2026-08-28.log`](perf-2026-08-28.log), so the table can be audited rather
than taken on trust. The advantage is the constraint lifecycle at both degrees,
and it grows with concurrency. Why it grows is not something this case measures.

This is a single-host trend on a shared machine, not a certified benchmark: the
same baseline work has been measured at 3.59 s and 6.07 s within one run, which is
exactly why the statistic is a ratio and not a stopwatch. The case prints every
pair, so measure your own hardware rather than trusting a number from someone
else's.

The only thing the case asserts about speed is a tripwire, not a claim: it fails
if the median ratio exceeds 1.5, which would mean importdb had become *slower*
than the thing it replaces. What it asserts on every pair, always, is correctness
parity — the two arms must agree with each other and with the source on the
catalog fingerprint and on every per-class row count. A disagreement about the
resulting database is a hard failure whatever the clock said.

## Where the time goes at ten times the data

The table above is measured below a threshold that changes the shape of the
answer. Scaling the same fixture ten times, to 3.93M rows, and timing importdb's
own phases:

| phase | 393K rows | 3.93M rows |
|---|---|---|
| load | 1.58 s &nbsp;(64 %) | 6.05 s &nbsp;(17 %) |
| rebuild | 0.45 s &nbsp;(18 %) | 10.23 s &nbsp;(29 %) |
| **FK define** | 0.18 s &nbsp;(7 %) | **17.97 s &nbsp;(51 %)** |
| stats | 0.09 s | 0.55 s |
| **total** | **2.45 s** | **34.93 s** |

FK definition is the largest phase in every run at the larger size — 49 % to 61 %
across four of them — with rebuild second. Together they are 74 % to 85 % of the
run, and **both are serial**: the only concurrency in the tool is the load phase's
pool of `cub_admin loaddb -C` children.

Which means `--degree` has little left to work on. At 3.93M rows it moves the
total from 37.91 s to 36.79 s, because the phase it parallelises is already the
small one:

| 3.93M rows | load | rebuild | FK define | total |
|---|---|---|---|---|
| degree 1 | 6.78 s | 11.87 s | 18.58 s | 37.91 s |
| degree 4 | 4.08 s | 8.63 s | 22.60 s | 36.79 s |

Single-arm runs — importdb only, no paired baseline — on a shared host, so read
the shares rather than the clock. The shift from 64 % to 17 % is a large effect
across a tenfold size change, not a between-run wobble.

## The cause is the page buffer

Not the serialism. One `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` over
3,000,000 child rows against a 30,000-row parent, nothing changed but one setting:

| `data_buffer_size` | ADD FK |
|---|---|
| 512 M &nbsp;(the engine default) | 19.11 s &nbsp;(25.73 s on a second run) |
| 1 G | **3.79 s** |
| 2 G | 2.31 s |

A sharp knee, not a curve. And it is a knee in the ratio of buffer to data, not in
the row count — the same statement over 300,000 rows takes 0.39 s and over
1,000,000 takes 1.03 s, both sub-linear, before 3,000,000 costs twenty-five times
what one million did.

Why the buffer decides it is in the engine: an FK build probes the parent's primary
key once per child row (`btree_prepare_bts`, `btree_load.c`). While the child heap,
the child's index and the parent's key fit in the page buffer together those probes
are memory; past that they are disk.

importdb says so before the load, comparing the dump's object bytes against the
server's `data_buffer_size`. Object bytes is a proxy for the working set and not a
measure of it — the same bytes are far more rows when the rows are narrow, and a
dump carries no row count — so the message reports the measured pair and leaves the
judgement with the operator.

## What was refuted

Three plausible answers were measured and are wrong. They are here because each
one would have been a large change aimed at the wrong thing.

**`sort_buffer_size` is not the lever.** Measured three times. Raising it from the
2 M default to 256 M appeared to take FK definition from 18.58 s to 13.51 s — until
the numbers were read against the `load` phase, which does not use the server's
sort buffer and had moved just as much. The host had gone quiet, nothing else:

| 3.93M rows, degree 1 | load *(control)* | FK define | FK define ÷ load |
|---|---|---|---|
| `sort_buffer_size=2M`, run 1 | 6.05 s | 17.97 s | 2.97 |
| `sort_buffer_size=2M`, run 2 | 6.78 s | 18.58 s | 2.74 |
| `sort_buffer_size=256M` | 4.32 s | 13.51 s | **3.13** |

Relative cost went slightly *up* for a 128-fold larger buffer. On the isolated
single-statement benchmark it is the same story: 23.28 s at 256 M against 19.11 s
at 2 M, within the noise of each other.

**Parallelising the terminal phases across sessions is aimed at the wrong thing,
and blocked besides.** Wrong because serialism is not where the cost is. Blocked
because the engine's own parallel index build — `btree_load.c` has one, with shard
workers — is gated to loaddb client types:

```c
bool bt_load_parallel_enabled (const LOAD_ARGS *load_args)
{ return load_args != NULL && load_args->no_redo; }

/* The client flag is only a request; the server decides. Restricting the no-redo
 * build to loaddb client types keeps it out of ordinary traffic no matter what a
 * client sends. */
eligible_no_redo = no_logging_index != 0
                   && BOOT_IS_LOADDB_CLIENT_TYPE (logtb_find_client_type (thread_p->tran_index));
```

importdb connects as `DB_CLIENT_TYPE_ADMIN_UTILITY`, which that macro does not
accept, and the client-side switch is not on the installed header surface. There is
a route — `cub_admin loaddb` does register `--no-logging-index`, undocumented in
`--help` — but it would reach only the deferred secondary indexes, not the PK and
UNIQUE constraints the rebuild phase replays, and those are the expensive ones on a
large table.

**Splitting one object file across loaders has a low ceiling.** The fan-out unit is
the object file, so one huge table is serial at any degree. That is a real gap, but
load is 17 % of a large run: a perfect fix bounds out there.

## Caveats

- **Small dumps do not benefit.** importdb's fixed setup — catalog snapshot,
  strip, rebuild, a per-file child process — is paid whatever the row count, so
  below some size it is *slower* than the thing it replaces. Where that crossover
  falls depends on your hardware and schema; measure before adopting it for small
  reloads.
- **`--degree` needs a `--datafile-per-class` dump.** The fan-out is over object
  files, so a default single-file dump runs serially no matter what you pass —
  silently, today.
- **The paired table is measured below the knee.** 393,000 rows never reaches the
  page-buffer threshold, so the 2.1× and 3.0× are honest for that size and say
  nothing about what happens above it. Nobody has run the paired comparison at
  3.93M rows; whether the advantage holds, grows, or inverts there is unmeasured.

## Reproducing any of this

```sh
tests/run_tests.sh perf                      # the paired table; ~30 minutes
PERF_FACTS=3000000 PERF_SIDES=300000 PERF_DIMS=30000 tests/run_tests.sh perf
```

The fixture is parameterised on all three table sizes, so the larger measurements
above need no code change — only time, and a host quiet enough to trust.
