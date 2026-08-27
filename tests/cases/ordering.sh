#!/bin/bash
# ordering -- does the planner order the load by its dependencies?
#
# The fixture has three levels of foreign-key edges below the root, one
# inheritance edge and a RANGE-partitioned table. The import must complete, and
# the level sets it prints must be topologically valid: every parent strictly
# below its child, every superclass strictly below its subclass. That check is
# done on the tool's own printed plan, which is what an operator reads.
set -uo pipefail
export CASE_NAME=ordering
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_ord_src
TGT=it_ord_tgt
DUMP="$WORK/dump"

db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" ordering || die "fixture DDL failed (see $WORK/ordering.ddl.log)"
fixture_rows "$SRC" ordering || die "fixture rows failed (see $WORK/ordering.rows.log)"

catalog_fp sa "$SRC" "$WORK/src.catalog"
data_fp sa "$SRC" "$WORK/src.data"
unload_dump "$SRC" "$DUMP" --datafile-per-class || die "unloaddb failed"

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"

run_import "$WORK/import.log" -u dba "$TGT" "$DUMP"
assert_rc "import exits clean" "$IT_RC" 0
assert_grep "report verdict is COMPLETE" "$WORK/import.log" "import COMPLETE"
assert_grep "all three FK edges defined" "$WORK/import.log" "defined 3 FK\\(s\\)"
assert_grep "the FK chain is four levels deep" "$WORK/import.log" "data phase 4 level\\(s\\)"

# Check the printed plan against the printed graph. Level-set lines are
# "      L<n>: [a, b]"; an FK edge is "child -> parent (name)" (4 fields) and an
# inheritance edge is "child -> super" (3). The terminal-task lines also carry
# "->" but always start with a #N task number, so the field count separates them.
awk '
/^[ \t]*L[0-9]+: \[/ {
  lvl = $1; sub(/^L/, "", lvl); sub(/:$/, "", lvl);
  s = $0; sub(/^[^[]*\[/, "", s); sub(/\].*$/, "", s);
  n = split(s, arr, /, */);
  for (i = 1; i <= n; i++) {
    if (arr[i] != "") {
      level[arr[i]] = lvl + 0;
      if (lvl + 0 > maxlvl) maxlvl = lvl + 0;
    }
  }
  next
}
NF == 4 && $2 == "->" { nfk++; fkc[nfk] = $1; fkp[nfk] = $3; next }
NF == 3 && $2 == "->" { nih++; ihc[nih] = $1; ihs[nih] = $3; next }
END {
  bad = 0;
  for (i = 1; i <= nfk; i++) {
    if (!(fkc[i] in level) || !(fkp[i] in level)) {
      printf "unplaced FK edge %s -> %s\n", fkc[i], fkp[i]; bad++; continue
    }
    if (level[fkp[i]] >= level[fkc[i]]) {
      printf "FK %s(L%d) not below %s(L%d)\n", fkp[i], level[fkp[i]], fkc[i], level[fkc[i]]; bad++
    }
  }
  for (i = 1; i <= nih; i++) {
    if (!(ihc[i] in level) || !(ihs[i] in level)) {
      printf "unplaced inheritance edge %s -> %s\n", ihc[i], ihs[i]; bad++; continue
    }
    if (level[ihs[i]] >= level[ihc[i]]) {
      printf "super %s(L%d) not below %s(L%d)\n", ihs[i], level[ihs[i]], ihc[i], level[ihc[i]]; bad++
    }
  }
  printf "levels=%d fk=%d inh=%d bad=%d\n", maxlvl + 1, nfk, nih, bad
}' "$WORK/import.log" > "$WORK/plan.check"

sed -n '1,8p' "$WORK/plan.check" | sed 's/^/      ordering     plan: /'
assert_eq "planned level sets are topologically valid" \
  "$(awk -F'bad=' '/^levels=/ { print $2 }' "$WORK/plan.check")" 0
assert_eq "every FK edge appears in the printed graph" \
  "$(awk -F'[= ]' '/^levels=/ { print $4 }' "$WORK/plan.check")" 3
assert_eq "the inheritance edge appears in the printed graph" \
  "$(awk -F'[= ]' '/^levels=/ { print $6 }' "$WORK/plan.check")" 1

# the partitioned table's three sub-classes are separate object files
assert_eq "partitioned table fully loaded" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM part_t")" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM part_t")"
assert_eq "all three partitions carry rows" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_partition WHERE class_name='part_t'")" 3

catalog_fp cs "$TGT" "$WORK/tgt.catalog"
data_fp cs "$TGT" "$WORK/tgt.data"
assert_same "catalog fingerprint is identical" "$WORK/src.catalog" "$WORK/tgt.catalog"
assert_same "row counts and checksums are identical" "$WORK/src.data" "$WORK/tgt.data"

case_exit
