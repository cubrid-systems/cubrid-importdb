#!/bin/bash
# fkcycle -- two tables that reference each other.
#
# cy_a.bid references cy_b.bid and cy_b.aid references cy_a.aid, so no load
# order satisfies both foreign keys. importdb drops the constraints before the
# data phase and defines them again on the populated tables, so the cycle is a
# non-event: it must import in one command, both FKs present at the end.
set -uo pipefail
export CASE_NAME=fkcycle
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_cyc_src
TGT=it_cyc_tgt
DUMP="$WORK/dump"

db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" fkcycle || die "fixture DDL failed"
fixture_rows "$SRC" fkcycle || die "fixture rows failed"

catalog_fp sa "$SRC" "$WORK/src.catalog"
data_fp sa "$SRC" "$WORK/src.data"
unload_dump "$SRC" "$DUMP" --datafile-per-class || die "unloaddb failed"

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"

run_import "$WORK/import.log" -u dba "$TGT" "$DUMP"
assert_rc "import of a cyclic schema exits clean" "$IT_RC" 0
assert_grep "report verdict is COMPLETE" "$WORK/import.log" "import COMPLETE"
assert_grep "the cycle is reported, not refused" "$WORK/import.log" "cycle: \\[cy_a, cy_b, cy_a\\]"
assert_grep "both FK edges accepted by the engine" "$WORK/import.log" "2 edge\\(s\\) clean"
assert_grep "both FK edges defined" "$WORK/import.log" "defined 2 FK\\(s\\)"

assert_eq "both FKs present in the target catalog" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_index WHERE class_name IN ('cy_a','cy_b') AND is_foreign_key='YES'")" 2
assert_eq "no withheld FK recorded in the manifest" \
  "$(awk '/^withheld: / { print $2; exit }' "$DUMP/importdb.manifest")" 0
# the mutual references are the point: every row must carry its counterpart
assert_eq "every cy_a row references a cy_b row" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM cy_a WHERE bid IS NOT NULL")" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM cy_a WHERE bid IS NOT NULL")"

catalog_fp cs "$TGT" "$WORK/tgt.catalog"
data_fp cs "$TGT" "$WORK/tgt.data"
assert_same "catalog fingerprint is identical" "$WORK/src.catalog" "$WORK/tgt.catalog"
assert_same "row counts and checksums are identical" "$WORK/src.data" "$WORK/tgt.data"

case_exit
