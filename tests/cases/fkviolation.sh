#!/bin/bash
# fkviolation -- a dump whose data does not satisfy its own foreign keys.
#
# Two children of one parent, each with a foreign key, each given orphan rows
# by editing the unloaddb object files. (The source cannot hold them: CUBRID's
# ALTER ... ADD FOREIGN KEY does check the rows already present.) What importdb
# must do:
#
#   * detect the orphans and enumerate the offending rows,
#   * exit non-zero by default, stopping at the first violated edge,
#   * WITHHOLD the violated FK -- it must be genuinely absent from the catalog,
#     with its repair DDL recorded in the manifest,
#   * with --continue, report every violated edge rather than the first,
#   * and still commit the loaded data, because that is what an operator
#     repairs from.
set -uo pipefail
export CASE_NAME=fkviolation
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_fkv_src
TGT_FF=it_fkv_ff
TGT_CO=it_fkv_co

db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" fkviolation || die "fixture DDL failed"
fixture_rows "$SRC" fkviolation || die "fixture rows failed"

# the source is consistent, and the engine proves it: adding the FK a second
# time over an orphan is refused. That is why the orphans go into the dump.
assert_eq "source has no orphans" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM fv_c1 WHERE pid NOT IN (SELECT id FROM fv_p)")" 0

unload_dump "$SRC" "$WORK/dump" --datafile-per-class || die "unloaddb failed"

# Inject the orphans: two on fv_c1, one on fv_c2. Each child object file is
# "%class [owner].[cls] (col...)" followed by one whitespace-separated row per
# line, so an appended line is an appended row.
inject () { # dumpdir
  printf '21 777 210\n22 778 220\n' >> "$1/${SRC}_dba.fv_c1_objects"
  printf '16 888 160\n' >> "$1/${SRC}_dba.fv_c2_objects"
}
cp -r "$WORK/dump" "$WORK/dump_ff"
cp -r "$WORK/dump" "$WORK/dump_co"
inject "$WORK/dump_ff"
inject "$WORK/dump_co"

# --------------------------------------------------------------- default: fail-fast

db_create "$TGT_FF" || die "cannot create $TGT_FF"
db_start "$TGT_FF" || die "cannot start $TGT_FF"
run_import "$WORK/ff.log" -u dba "$TGT_FF" "$WORK/dump_ff"
FF_RC=$IT_RC

assert_nonzero_rc "orphan dump fails the run by default" "$FF_RC"
assert_grep "the rejected edge and its orphan count are named" "$WORK/ff.log" \
  "found 2 orphan row\\(s\\) on 'fv_c1' -> 'fv_p' \\[fk_c1_p\\]"
assert_grep "the default policy is announced as fail-fast" "$WORK/ff.log" "fail-fast"
assert_grep "report verdict is PARTIAL" "$WORK/ff.log" "import PARTIAL"

assert_file "an exceptions artifact was written" "$WORK/dump_ff/importdb.exceptions"
assert_grep "the exceptions artifact records the policy" "$WORK/dump_ff/importdb.exceptions" \
  "^policy: fail-fast"
assert_grep "offending row 21 is enumerated" "$WORK/dump_ff/importdb.exceptions" \
  "^orphan: child\\[cid=21\\] fk\\[pid=777\\]"
assert_grep "offending row 22 is enumerated" "$WORK/dump_ff/importdb.exceptions" \
  "^orphan: child\\[cid=22\\] fk\\[pid=778\\]"
assert_eq "fail-fast stopped after the first violated edge" \
  "$(awk '/^violated_edges: / { print $2; exit }' "$WORK/dump_ff/importdb.exceptions")" 1

# the withheld FK must be genuinely absent, not merely reported
assert_eq "no FK defined on either child" \
  "$(q1 cs "$TGT_FF" "SELECT count(*) FROM db_index WHERE class_name IN ('fv_c1','fv_c2') AND is_foreign_key='YES'")" 0
assert_eq "the parent PK is still rebuilt" \
  "$(q1 cs "$TGT_FF" "SELECT count(*) FROM db_index WHERE class_name='fv_p' AND is_primary_key='YES'")" 1
assert_eq "the loaded data is committed, orphans included" \
  "$(q1 cs "$TGT_FF" "SELECT count(*) FROM fv_c1")" 22
assert_eq "the orphan rows are the ones an operator has to repair" \
  "$(q1 cs "$TGT_FF" "SELECT count(*) FROM fv_c1 WHERE pid NOT IN (SELECT id FROM fv_p)")" 2

assert_eq "manifest records both FKs withheld" \
  "$(awk '/^\[fkdefine\]/ { s = 1 } s && /^withheld: / { print $2; exit }' "$WORK/dump_ff/importdb.manifest")" 2
assert_grep "manifest carries the repair DDL for fk_c1_p" "$WORK/dump_ff/importdb.manifest" \
  "^readd: ALTER CLASS \\[dba\\]\\.\\[fv_c1\\] ADD CONSTRAINT \\[fk_c1_p\\] FOREIGN KEY"
assert_grep "manifest carries the repair DDL for fk_c2_p" "$WORK/dump_ff/importdb.manifest" \
  "^readd: ALTER CLASS \\[dba\\]\\.\\[fv_c2\\] ADD CONSTRAINT \\[fk_c2_p\\] FOREIGN KEY"

# the recorded DDL has to be the real repair, so run it after fixing the data
sql_cs "$TGT_FF" "DELETE FROM fv_c1 WHERE pid NOT IN (SELECT id FROM fv_p);
                  DELETE FROM fv_c2 WHERE pid NOT IN (SELECT id FROM fv_p)" > "$WORK/repair.log" 2>&1
awk '/^readd: / { sub(/^readd: /, ""); print }' "$WORK/dump_ff/importdb.manifest" > "$WORK/readd.sql"
sql_cs_script "$TGT_FF" "$WORK/readd.sql" >> "$WORK/repair.log" 2>&1
assert_eq "the recorded re-add DDL defines both FKs once the data is repaired" \
  "$(q1 cs "$TGT_FF" "SELECT count(*) FROM db_index WHERE class_name IN ('fv_c1','fv_c2') AND is_foreign_key='YES'")" 2

db_drop "$TGT_FF"

# ------------------------------------------------------------------- --continue

db_create "$TGT_CO" || die "cannot create $TGT_CO"
db_start "$TGT_CO" || die "cannot start $TGT_CO"
run_import "$WORK/co.log" -u dba --continue "$TGT_CO" "$WORK/dump_co"
CO_RC=$IT_RC

assert_nonzero_rc "--continue still fails the run" "$CO_RC"
assert_grep "--continue reports every rejected edge" "$WORK/co.log" \
  "2 FOREIGN KEY\\(s\\) rejected by the engine, 3 orphan row\\(s\\) total"
assert_eq "manifest records two violated edges" \
  "$(awk '/^\[validate\]/ { s = 1 } s && /^violated: / { print $2; exit }' "$WORK/dump_co/importdb.manifest")" 2
assert_eq "exceptions artifact records two violated edges" \
  "$(awk '/^violated_edges: / { print $2; exit }' "$WORK/dump_co/importdb.exceptions")" 2
assert_eq "exceptions artifact enumerates all three orphans" \
  "$(grep -c '^orphan: ' "$WORK/dump_co/importdb.exceptions")" 3
assert_grep "the second edge is enumerated, not just skipped" "$WORK/dump_co/importdb.exceptions" \
  "^\\[edge\\] fv_c2 -> fv_p \\(fk_c2_p\\)"
assert_eq "still no FK defined on either child" \
  "$(q1 cs "$TGT_CO" "SELECT count(*) FROM db_index WHERE class_name IN ('fv_c1','fv_c2') AND is_foreign_key='YES'")" 0

# The closing catalog check must not fire here: these FKs are absent from the
# catalog on purpose and recorded as withheld, which is what excuses them. If it
# fired, a legitimately partial run would be reported as an importdb defect.
assert_eq "the closing catalog check stays silent over a withheld FK" \
  "$(cat "$WORK"/*.log | grep -c 'does not hold everything this run recorded as restored' || true)" 0

case_exit
