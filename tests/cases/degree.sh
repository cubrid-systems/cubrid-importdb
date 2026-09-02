#!/bin/bash
# degree -- --degree=1|2|4 must all reach the same final state.
#
# The data phase spawns `cub_admin loaddb -C` children through a bounded pool at
# every degree, so degree 1 is that pool with one slot rather than a separate
# implementation. What --degree changes is how many object files are in flight
# at once, on independent connections and independent transactions. The claim
# under test is that the bound does not change the outcome: five object files
# loaded one at a time, two at a time and four at a time must produce the same
# catalog, the same row counts and the same content checksums -- and the same as
# the source.
#
# Each degree gets its own copy of the dump, because a manifest left by one run
# would make the next one a no-op resume.
set -uo pipefail
export CASE_NAME=degree
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_deg_src
ROWS=2000

db_create "$SRC" 128M 64M || die "cannot create $SRC"
fixture_apply "$SRC" wide || die "fixture DDL failed"
fixture_rows "$SRC" wide "$ROWS" || die "fixture rows failed"
catalog_fp sa "$SRC" "$WORK/src.catalog"
data_fp sa "$SRC" "$WORK/src.data"
unload_dump "$SRC" "$WORK/dump" --datafile-per-class || die "unloaddb failed"
assert_eq "the dump has five object files to spread over" \
  "$(find "$WORK/dump" -name '*_objects' | wc -l | tr -d ' ')" 5

for d in 1 2 4; do
  tgt="it_deg_d$d"
  dump="$WORK/dump_d$d"
  cp -r "$WORK/dump" "$dump"
  db_create "$tgt" 128M 64M || die "cannot create $tgt"
  db_start "$tgt" || die "cannot start $tgt"
  run_import "$WORK/d$d.log" -u dba --degree="$d" "$tgt" "$dump"
  assert_rc "degree $d import exits clean" "$IT_RC" 0
  assert_grep "degree $d reports COMPLETE" "$WORK/d$d.log" "import COMPLETE"
  # The two load-completion messages differ by one clause, and which one appears
  # is the only evidence in the output that the degree reached the loader.
  if [ "$d" = "1" ]; then
    assert_grep "degree 1 uses the serial completion wording" "$WORK/d$d.log" \
      "loaded [0-9]+ row\\(s\\) from 5 object file\\(s\\) into '$tgt'\\."
    assert_no_grep "and does not name a degree" "$WORK/d$d.log" "at degree"
  else
    assert_grep "degree $d reports the parallel completion wording" "$WORK/d$d.log" \
      "loaded [0-9]+ row\\(s\\) from 5 object file\\(s\\) into '$tgt' at degree $d\\."
  fi
  catalog_fp cs "$tgt" "$WORK/d$d.catalog"
  data_fp cs "$tgt" "$WORK/d$d.data"
  assert_same "degree $d round-trips the source catalog" "$WORK/src.catalog" "$WORK/d$d.catalog"
  assert_same "degree $d round-trips the source rows" "$WORK/src.data" "$WORK/d$d.data"
  db_drop "$tgt"
done

assert_same "degree 1 and degree 2 agree on the final catalog" "$WORK/d1.catalog" "$WORK/d2.catalog"
assert_same "degree 1 and degree 4 agree on the final catalog" "$WORK/d1.catalog" "$WORK/d4.catalog"
assert_same "degree 1 and degree 2 agree on the final rows" "$WORK/d1.data" "$WORK/d2.data"
assert_same "degree 1 and degree 4 agree on the final rows" "$WORK/d1.data" "$WORK/d4.data"

case_exit
