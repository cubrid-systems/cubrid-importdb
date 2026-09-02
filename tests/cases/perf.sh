#!/bin/bash
# perf -- importdb against a loaddb reload with the parallelism held equal.
#
# The comparison this case exists to make honestly. importdb's --degree=N spawns
# up to N concurrent `cub_admin loaddb -C -d <object-file>` children, one per
# object file, so a baseline of ONE serial `loaddb -d` over the whole dump would
# be measuring two things at once -- the constraint lifecycle and the
# parallelism -- and would credit importdb for both. So the baseline fans out
# too, over the same object files, through a pool with the same bound:
#
#   arm L (loaddb, parallelism-matched, at degree N)
#     1. loaddb -s <prefix>_schema        -- classes, PK, UNIQUE and FK, all live
#     2. N concurrent loaddb -d <object-file>, one per file, bounded pool of N
#     3. loaddb -i <prefix>_indexes       -- the plain secondary index
#
#   arm I (importdb, at the same degree N)
#     cubrid-importdb -u dba --degree=N <db> <dump-dir>
#
# Schema-first in arm L is not a straw man: it is what a hand-scripted loaddb
# reload does, and it is exactly the thing importdb changes. Every PK, UNIQUE
# and FK is defined before the first row lands, so each row pays per-row b-tree
# maintenance and a referential check; importdb strips the constraints, loads
# into bare heaps, and bulk-builds them afterwards. Both arms run every phase in
# CS mode against the same running server -- switching arm L's schema and index
# phases to SA mode would force a server stop and start into the timed region
# and measure process startup instead of the lifecycle. Both arms also update
# statistics: loaddb does it per object file, importdb once at the end.
#
# WHAT IS ASSERTED, AND WHAT IS ONLY REPORTED
#
# Correctness parity is asserted, always: after every pair the two arms must
# agree on the catalog fingerprint (so the same constraint and index set) and on
# the per-class row counts, and both must agree with the source. A disagreement
# about the resulting database is a hard failure whatever the clock said.
#
# The timings are REPORTED AS NUMBERS, not as pass/fail. This is a shared, noisy
# host -- the same baseline work has been measured at 6.09 s, 9.40 s and 10.02 s
# in one afternoon -- so a before/after subtraction is not readable and a timing
# assertion would flake. The design is paired instead: after one unmeasured
# warmup pair, each iteration runs both arms back to back, seconds apart, on the
# same dump; the reported statistic is the MEDIAN OF THE PER-PAIR RATIOS,
# because drift that moves both arms of a pair barely moves their ratio. Every
# pair is printed so one bad pair is visible rather than averaged away, and the
# order alternates (L,I then I,L, ...) so within-pair drift cancels.
#
# There is exactly ONE performance assertion, and it is deliberately loose: fail
# if the median ratio importdb/loaddb exceeds PERF_GUARD (1.5). It is not
# checking that importdb is fast -- it is protecting against a real regression
# in the constraint lifecycle, the kind that shows up as importdb becoming
# SLOWER than the thing it replaces. importdb has measured 2-4x faster
# elsewhere, so 1.5 leaves a wide band of host noise below the tripwire; a few
# percent of drift must never turn this red. If the host is too loaded to
# produce a readable measurement at all -- the spread across pairs is enormous,
# or the arms are so short that process startup dominates -- the guard is
# SKIPPED with the reason printed, and the parity assertions still stand.
#
# THREE DELIBERATE DEVIATIONS FROM THE OBVIOUS SETUP, all three measured
#
#   * One pair per degree is a WARMUP and is not counted. The first load into a
#     freshly created volume touches pages nothing has written yet, and across
#     two runs that first-touch cost landed on pair 1 as a visible outlier in
#     both arms and at both degrees (ratio 0.80 / 0.76 against a 0.37-0.60
#     plateau). It is a property of the volume, not of either tool. The warmup
#     pair is still a real import, and it is where both arms are checked against
#     the source.
#   * The two target databases are created once per degree and then RESET to
#     empty (every user class dropped) before each arm, rather than re-created
#     per arm. `cubrid createdb` costs ~10 s here and 20 of them would eat the
#     case's whole budget -- but the real reason is noise: a volume that
#     auto-extends mid-load took 11-17 s for work that takes 5-6 s in a
#     pre-sized volume, and the extension does not land in the same place in
#     both arms. Pre-sizing generously and resetting gives both arms an
#     identical, extension-free starting state. Both arms get exactly the same
#     treatment, and the parity assertions still compare full final states.
#   * The dump is unloaded with --datafile-per-class. A default unloaddb writes
#     ONE object file, which would leave --degree nothing to spread over and
#     make "parallelism held equal" vacuous for both arms; the case asserts the
#     file count so a change upstream cannot silently hollow it out.
#
# Tunables, for a host that cannot afford the default budget:
#   PERF_FACTS PERF_SIDES PERF_DIMS   fixture size
#   PERF_PAIRS                        pairs per degree (>= 5 to be readable)
#   PERF_DEGREES                      degrees to compare at
#   PERF_GUARD PERF_SPREAD PERF_FLOOR guard threshold / readability limits
set -uo pipefail
export CASE_NAME=perf
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

PERF_FACTS=${PERF_FACTS:-300000}
PERF_SIDES=${PERF_SIDES:-30000}
PERF_DIMS=${PERF_DIMS:-3000}
PERF_PAIRS=${PERF_PAIRS:-5}
PERF_DEGREES=${PERF_DEGREES:-"1 4"}
# median ratio importdb/loaddb above this is a lifecycle regression, not noise
PERF_GUARD=${PERF_GUARD:-1.5}
# max/min ratio above this means the host was too noisy to read
PERF_SPREAD=${PERF_SPREAD:-3.0}
# median loaddb arm shorter than this is dominated by process startup
PERF_FLOOR=${PERF_FLOOR:-2.0}

SRC=it_perf_src
LDB=it_perf_l
IDB=it_perf_i
DUMP="$WORK/dump"

# ------------------------------------------------------------------ arithmetic

# Wall clock with sub-second resolution; awk does the arithmetic so the case
# does not depend on bc.
now () {
  date +%s.%N
}

elapsed () { # start end
  awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", b - a }'
}

ratio () { # numerator denominator
  awk -v i="$1" -v l="$2" 'BEGIN { if (l + 0 <= 0) { print "0.000"; exit } printf "%.3f", i / l }'
}

# Median, not mean: one bad pair must not move the summary.
median () { # value...
  printf '%s\n' "$@" | LC_ALL=C sort -g | awk '
    { v[NR] = $1 + 0 }
    END {
      if (NR == 0) { print "0.000"; exit }
      if (NR % 2) { printf "%.3f", v[(NR + 1) / 2] }
      else        { printf "%.3f", (v[NR / 2] + v[NR / 2 + 1]) / 2 }
    }'
}

gt () { # a b -- true when a > b
  awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 > b + 0) }'
}

# ----------------------------------------------------------------------- arms

# Drop every class the fixture defines, so the next arm starts from an empty
# target -- which is also what importdb insists on. Children before the parent,
# because an FK still points at pf_dim. IF EXISTS so a target left half-loaded
# by a failed arm still resets.
reset_target () { # db
  sql_cs "$1" "DROP TABLE IF EXISTS pf_fact;
               DROP TABLE IF EXISTS pf_side1;
               DROP TABLE IF EXISTS pf_side2;
               DROP TABLE IF EXISTS pf_side3;
               DROP TABLE IF EXISTS pf_dim" > "$WORK/reset.log" 2>&1
  if [ "$(q1 cs "$1" "SELECT count(*) FROM db_class WHERE is_system_class='NO'")" != "0" ]; then
    return 1
  fi
  return 0
}

# The parallelism-matched baseline. The pool is `wait -n`, which releases a slot
# as soon as ANY child finishes -- the same "next free slot" discipline
# import_load.cpp uses, not a FIFO that would idle behind the slowest file.
arm_loaddb () { # db degree logfile
  local db=$1 n=$2 log=$3 running=0 rc=0 f
  : > "$log"
  cubrid loaddb -C -u dba -s "$DUMP/${SRC}_schema" "$db" >> "$log" 2>&1 || rc=1
  for f in "${OBJ[@]}"; do
    cubrid loaddb -C -u dba -d "$f" "$db" >> "$log" 2>&1 &
    running=$((running + 1))
    if [ "$running" -ge "$n" ]; then
      wait -n || rc=1
      running=$((running - 1))
    fi
  done
  while [ "$running" -gt 0 ]; do
    wait -n || rc=1
    running=$((running - 1))
  done
  cubrid loaddb -C -u dba -i "$DUMP/${SRC}_indexes" "$db" >> "$log" 2>&1 || rc=1
  return "$rc"
}

# The tool under test, on the same dump. The manifest a previous run left behind
# would make this one a resume no-op, so it is removed first -- outside the
# timed region, together with the target reset.
arm_importdb_prepare () {
  rm -f "$DUMP/importdb.manifest" "$DUMP/importdb.exceptions"
}

arm_importdb () { # db degree logfile
  "$IMPORTDB_BIN" -u dba --degree="$2" "$1" "$DUMP" > "$3" 2>&1
}

# One pair: both arms, back to back, in the requested order. Each target is
# reset immediately before ITS OWN arm, never both up front -- dropping five
# populated classes leaves the server deallocating pages in the background, and
# an arm that starts into that pays for it. Resetting both first put the whole
# cost on whichever arm went first, which showed up as a clean systematic split:
# ratios 0.40-0.59 on L-first pairs against 1.31-1.45 on I-first pairs, from the
# order alone.
#
# Results land in PAIR_L / PAIR_I (seconds) and PAIR_LRC / PAIR_IRC.
run_pair () { # degree tag first-arm(L|I)
  local n=$1 tag=$2 ord=$3 t0 t1
  if [ "$ord" = "L" ]; then
    reset_target "$LDB" || die "cannot reset $LDB"
    t0=$(now); arm_loaddb "$LDB" "$n" "$WORK/l.$tag.log"; PAIR_LRC=$?; t1=$(now)
    PAIR_L=$(elapsed "$t0" "$t1")
    reset_target "$IDB" || die "cannot reset $IDB"
    arm_importdb_prepare
    t0=$(now); arm_importdb "$IDB" "$n" "$WORK/i.$tag.log"; PAIR_IRC=$?; t1=$(now)
    PAIR_I=$(elapsed "$t0" "$t1")
  else
    reset_target "$IDB" || die "cannot reset $IDB"
    arm_importdb_prepare
    t0=$(now); arm_importdb "$IDB" "$n" "$WORK/i.$tag.log"; PAIR_IRC=$?; t1=$(now)
    PAIR_I=$(elapsed "$t0" "$t1")
    reset_target "$LDB" || die "cannot reset $LDB"
    t0=$(now); arm_loaddb "$LDB" "$n" "$WORK/l.$tag.log"; PAIR_LRC=$?; t1=$(now)
    PAIR_L=$(elapsed "$t0" "$t1")
  fi
}

# The catalog and the per-class row counts of both targets, as they stand.
snapshot_arms () { # tag
  catalog_fp cs "$LDB" "$WORK/l.$1.catalog"
  catalog_fp cs "$IDB" "$WORK/i.$1.catalog"
  row_counts cs "$LDB" "$WORK/l.$1.rowcounts"
  row_counts cs "$IDB" "$WORK/i.$1.rowcounts"
}

# --------------------------------------------------------------- the fixture

note "host at start: $(uptime)"

db_create "$SRC" 768M 256M || die "cannot create $SRC"
fixture_apply "$SRC" perf || die "fixture DDL failed"
bash "$IT_FIXTURES_DIR/gen_perf_rows.sh" "$PERF_FACTS" "$PERF_SIDES" "$PERF_DIMS" \
  > "$WORK/perf.rows.sql" || die "row generation failed"
sql_sa_load "$SRC" "$WORK/perf.rows.sql" > "$WORK/perf.rows.log" 2>&1
grep -qi 'ERROR' "$WORK/perf.rows.log" && die "fixture rows failed (see $WORK/perf.rows.log)"

catalog_fp sa "$SRC" "$WORK/src.catalog"
row_counts sa "$SRC" "$WORK/src.rowcounts"
src_total=$(awk -F'rows=' '{ split($2, a, " "); s += a[1] } END { print s + 0 }' "$WORK/src.rowcounts")

unload_dump "$SRC" "$DUMP" --datafile-per-class || die "unloaddb failed"
OBJ=()
while IFS= read -r f; do
  OBJ+=("$f")
done < <(find "$DUMP" -name '*_objects' | LC_ALL=C sort)

# --datafile-per-class is load-bearing for the whole comparison: with one object
# file neither arm can fan out and --degree means nothing.
assert_eq "the dump fans out over one object file per class" "${#OBJ[@]}" 5
assert_file "the dump carries a schema file" "$DUMP/${SRC}_schema"
assert_file "the dump carries an index file" "$DUMP/${SRC}_indexes"

# The source has done its job; drop it so its server and its ~500 MB of volume
# are not competing with the arms being measured.
db_drop "$SRC"

note "fixture: $src_total row(s) over ${#OBJ[@]} object file(s), $PERF_FACTS of them in pf_fact"
note "dump size: $(du -sh "$DUMP" | cut -f1)"

# -------------------------------------------------------------- the pairs

read -r -a degrees <<< "$PERF_DEGREES"
for n in "${degrees[@]}"; do
  # Fresh databases per degree: pre-sized so neither arm extends a volume
  # mid-load, and re-made between degrees so ten loads' worth of archive logs do
  # not accumulate under the second degree's measurement.
  db_create "$LDB" 512M 128M || die "cannot create $LDB"
  db_create "$IDB" 512M 128M || die "cannot create $IDB"
  db_start "$LDB" || die "cannot start $LDB"
  db_start "$IDB" || die "cannot start $IDB"

  note "--- degree $n: 1 warmup pair + $PERF_PAIRS counted pair(s), arm order alternating ---"

  # One unmeasured warmup pair. The first load into a freshly created volume
  # touches pages nothing has written yet, and that first-touch cost landed
  # entirely on pair 1: ratio 0.80 and 0.76 against a 0.37-0.60 plateau across
  # two runs, in both arms and at both degrees. It is an artifact of the volume,
  # not a property of either tool, so it is warmed away rather than averaged in.
  # The warmup is still a real import, and it is where both arms are checked
  # against the source.
  run_pair "$n" "n$n.warm" L
  note "degree $n warmup (not counted)  loaddb ${PAIR_L}s  importdb ${PAIR_I}s"
  assert_rc "degree $n warmup: the loaddb arm exits clean" "$PAIR_LRC" 0
  assert_rc "degree $n warmup: the importdb arm exits clean" "$PAIR_IRC" 0
  snapshot_arms "n$n.warm"
  assert_same "degree $n: the loaddb arm reproduces the source catalog" \
    "$WORK/src.catalog" "$WORK/l.n$n.warm.catalog"
  assert_same "degree $n: the importdb arm reproduces the source catalog" \
    "$WORK/src.catalog" "$WORK/i.n$n.warm.catalog"
  assert_same "degree $n: the loaddb arm reproduces the source row counts" \
    "$WORK/src.rowcounts" "$WORK/l.n$n.warm.rowcounts"
  assert_same "degree $n: the importdb arm reproduces the source row counts" \
    "$WORK/src.rowcounts" "$WORK/i.n$n.warm.rowcounts"
  assert_grep "degree $n: the importdb arm reports COMPLETE" \
    "$WORK/i.n$n.warm.log" "import COMPLETE"
  assert_grep "degree $n: the importdb arm loaded every source row" \
    "$WORK/i.n$n.warm.log" "loaded $src_total row\\(s\\)"

  ratios=()
  l_times=()
  i_times=()
  p=1
  while [ "$p" -le "$PERF_PAIRS" ]; do
    if [ $((p % 2)) -eq 1 ]; then
      first=L
    else
      first=I
    fi
    run_pair "$n" "n$n.p$p" "$first"

    r=$(ratio "$PAIR_I" "$PAIR_L")
    printf '      %-12s degree %s pair %s  loaddb %6ss  importdb %6ss  ratio I/L %s  (%s first)\n' \
      "$CASE_NAME" "$n" "$p" "$PAIR_L" "$PAIR_I" "$r" "$first"

    assert_rc "degree $n pair $p: the loaddb arm exits clean" "$PAIR_LRC" 0
    assert_rc "degree $n pair $p: the importdb arm exits clean" "$PAIR_IRC" 0

    # Correctness parity, asserted every pair: if the two arms disagree about
    # the database they produced, the timings are beside the point.
    snapshot_arms "n$n.p$p"
    assert_same "degree $n pair $p: both arms end with the same catalog" \
      "$WORK/l.n$n.p$p.catalog" "$WORK/i.n$n.p$p.catalog"
    assert_same "degree $n pair $p: both arms end with the same row counts" \
      "$WORK/l.n$n.p$p.rowcounts" "$WORK/i.n$n.p$p.rowcounts"

    ratios+=("$r")
    l_times+=("$PAIR_L")
    i_times+=("$PAIR_I")
    p=$((p + 1))
  done

  med=$(median "${ratios[@]}")
  med_l=$(median "${l_times[@]}")
  med_i=$(median "${i_times[@]}")
  lo=$(printf '%s\n' "${ratios[@]}" | LC_ALL=C sort -g | head -1)
  hi=$(printf '%s\n' "${ratios[@]}" | LC_ALL=C sort -g | tail -1)
  spread=$(ratio "$hi" "$lo")
  speedup=$(ratio "$med_l" "$med_i")

  note "degree $n: median loaddb ${med_l}s, median importdb ${med_i}s"
  note "degree $n: MEDIAN RATIO importdb/loaddb = $med (importdb ${speedup}x), pairs $lo..$hi"
  note "degree $n: single-host trend on a shared machine -- not a certified benchmark"

  # The one performance assertion, and the two conditions under which it is not
  # readable enough to make.
  if gt "$spread" "$PERF_SPREAD"; then
    skip "degree $n performance guard: pair ratios spread ${lo}..${hi} (x$spread, limit
      x$PERF_SPREAD) -- this host was too loaded during the run for the median to
      mean anything. The numbers above are still printed; the parity assertions
      above still hold."
  elif ! gt "$med_l" "$PERF_FLOOR"; then
    skip "degree $n performance guard: median loaddb arm ${med_l}s is under the
      ${PERF_FLOOR}s floor, so process startup dominates the measurement. Raise
      PERF_FACTS to make the arms long enough to compare."
  elif gt "$med" "$PERF_GUARD"; then
    fail "degree $n: median ratio importdb/loaddb $med exceeds the $PERF_GUARD guard -- importdb is slower than the parallelism-matched loaddb reload"
  else
    pass "degree $n: median ratio importdb/loaddb $med is within the $PERF_GUARD guard"
  fi

  db_drop "$LDB"
  db_drop "$IDB"
done

note "host at end: $(uptime)"

case_exit
