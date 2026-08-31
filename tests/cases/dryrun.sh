#!/bin/bash
# dryrun -- --dry-run prints the plan and changes nothing.
#
# The interesting part of the claim is that the dry run gets far enough to have
# changed something: it defines the whole schema on a real session and reads the
# catalog back to build the graph, then aborts. So the case asserts both halves
# -- the plan really was produced, and the target really is untouched -- and
# then imports for real into the same database, which only succeeds if the
# rollback was complete.
set -uo pipefail
export CASE_NAME=dryrun
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_dry_src
TGT=it_dry_tgt
DUMP="$WORK/dump"

db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" roundtrip || die "fixture DDL failed"
fixture_rows "$SRC" roundtrip || die "fixture rows failed"
catalog_fp sa "$SRC" "$WORK/src.catalog"
data_fp sa "$SRC" "$WORK/src.data"
unload_dump "$SRC" "$DUMP" --datafile-per-class || die "unloaddb failed"

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"

# a full catalog snapshot of the empty target, plus the two catalog tables a
# dry run would touch outside db_class
snapshot () { # outfile
  catalog_fp cs "$TGT" "$1"
  {
    printf '#COUNT classes %s\n' \
      "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE is_system_class='NO'")"
    printf '#COUNT serials %s\n' "$(q1 cs "$TGT" "SELECT count(*) FROM db_serial")"
    printf '#COUNT triggers %s\n' "$(q1 cs "$TGT" "SELECT count(*) FROM db_trigger")"
    printf '#COUNT indexes %s\n' \
      "$(q1 cs "$TGT" "SELECT count(*) FROM db_index i, db_class c WHERE i.class_name=c.class_name AND c.is_system_class='NO'")"
  } >> "$1"
}

snapshot "$WORK/before.catalog"
run_import "$WORK/dry.log" -u dba --dry-run "$TGT" "$DUMP"
assert_rc "dry run exits clean" "$IT_RC" 0
snapshot "$WORK/after.catalog"

# it really planned
assert_grep "the plan was printed" "$WORK/dry.log" "schedule for '$TGT' -- data phase"
assert_grep "the graph was built from a real catalog read" "$WORK/dry.log" \
  "dependency graph for '$TGT' -- 3 node\\(s\\), 1 FK edge\\(s\\)"
assert_grep "the terminal tasks were listed" "$WORK/dry.log" "terminal tasks"
assert_grep "the dry run says so at the end" "$WORK/dry.log" "DRY RUN"
assert_grep "the target is declared untouched" "$WORK/dry.log" "target database is untouched"
# ... and stopped before the first execution step
assert_no_grep "nothing was stripped" "$WORK/dry.log" "stripped [0-9]+ constraint"
assert_no_grep "nothing was loaded" "$WORK/dry.log" "loaded [0-9]+ row"

# it really changed nothing
assert_same "catalog is unchanged" "$WORK/before.catalog" "$WORK/after.catalog"
assert_eq "no user class exists" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE is_system_class='NO'")" 0
assert_no_file "no manifest was written" "$DUMP/importdb.manifest"
assert_no_file "no exceptions artifact was written" "$DUMP/importdb.exceptions"

# a second dry run is equally harmless
run_import "$WORK/dry2.log" -u dba --dry-run "$TGT" "$DUMP"
assert_rc "a second dry run exits clean" "$IT_RC" 0
snapshot "$WORK/after2.catalog"
assert_same "catalog still unchanged after a second dry run" \
  "$WORK/before.catalog" "$WORK/after2.catalog"

# the strongest evidence the rollback was complete: the real import into the
# same database still works, which it cannot if a class was left behind
run_import "$WORK/real.log" -u dba "$TGT" "$DUMP"
assert_rc "the real import into the same target still succeeds" "$IT_RC" 0
catalog_fp cs "$TGT" "$WORK/tgt.catalog"
data_fp cs "$TGT" "$WORK/tgt.data"
assert_same "and round-trips the source" "$WORK/src.catalog" "$WORK/tgt.catalog"
assert_same "with the same rows" "$WORK/src.data" "$WORK/tgt.data"

case_exit
