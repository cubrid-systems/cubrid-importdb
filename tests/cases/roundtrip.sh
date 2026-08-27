#!/bin/bash
# roundtrip -- does the import reproduce the source?
#
# The core claim. A source database carrying every constraint kind, index kind
# and column-type family the fixture can name is unloaded, imported into an
# empty database, and then compared against the source two ways:
#
#   * the catalog, by a tagged fingerprint over db_class / db_attribute /
#     db_index / db_index_key / db_direct_super_class / db_partition /
#     db_serial / db_trigger. It must be byte-identical.
#   * the data, per class, by row count and by an order-independent content
#     checksum (the rows are sorted before hashing, because the physical load
#     order of an import differs from the source's).
set -uo pipefail
export CASE_NAME=roundtrip
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_rt_src
TGT=it_rt_tgt
DUMP="$WORK/dump"

db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" roundtrip || die "fixture DDL failed (see $WORK/roundtrip.ddl.log)"
fixture_rows "$SRC" roundtrip || die "fixture rows failed (see $WORK/roundtrip.rows.log)"

catalog_fp sa "$SRC" "$WORK/src.catalog"
data_fp sa "$SRC" "$WORK/src.data"
unload_dump "$SRC" "$DUMP" --datafile-per-class || die "unloaddb failed"

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"

run_import "$WORK/import.log" -u dba "$TGT" "$DUMP"
assert_rc "import exits clean" "$IT_RC" 0
assert_grep "report verdict is COMPLETE" "$WORK/import.log" "import COMPLETE"
assert_grep "catalog reported to match the dump snapshot" "$WORK/import.log" \
  "full round-trip complete"

expected_rows=$(awk -F'rows=' '{ split($2, a, " "); s += a[1] } END { print s + 0 }' "$WORK/src.data")
assert_grep "loaded row total matches the source" "$WORK/import.log" \
  "loaded $expected_rows row\\(s\\)"

catalog_fp cs "$TGT" "$WORK/tgt.catalog"
data_fp cs "$TGT" "$WORK/tgt.data"

# split the two halves out so a failure names which one moved
for side in src tgt; do
  awk '/^#IDX|^#IKEY/' "$WORK/$side.catalog" > "$WORK/$side.indexes"
  awk '{ print $1, $2 }' "$WORK/$side.data" > "$WORK/$side.counts"
  awk '{ print $1, $3 }' "$WORK/$side.data" > "$WORK/$side.sums"
done

# and the counts asked of the server directly, not derived from the rows above
row_counts sa "$SRC" "$WORK/src.rowcounts"
row_counts cs "$TGT" "$WORK/tgt.rowcounts"

assert_same "catalog fingerprint is identical" "$WORK/src.catalog" "$WORK/tgt.catalog"
assert_same "constraint + index set is identical" "$WORK/src.indexes" "$WORK/tgt.indexes"
assert_same "row count per class is identical" "$WORK/src.counts" "$WORK/tgt.counts"
assert_same "and identical when counted by the server" \
  "$WORK/src.rowcounts" "$WORK/tgt.rowcounts"
assert_same "the two counts agree with each other on the target" \
  "$WORK/tgt.counts" "$WORK/tgt.rowcounts"
assert_same "content checksum per class is identical" "$WORK/src.sums" "$WORK/tgt.sums"

# a few of the fingerprint's claims called out by name, so a diff-only failure
# does not hide which object kind was lost
assert_eq "foreign key defined on rt_emp" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_index WHERE class_name='rt_emp' AND is_foreign_key='YES'")" 1
assert_eq "reverse index survived" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_index WHERE class_name='rt_emp' AND is_reverse='YES'")" 1
assert_eq "multi-column unique index survived" \
  "$(q1 cs "$TGT" "SELECT key_count FROM db_index WHERE index_name='u_emp_id_nm'")" 2
serial_cur=$(q1 sa "$SRC" "SELECT current_val FROM db_serial WHERE name='rt_seq'")
assert_eq "serial kept its advanced current value" \
  "$(q1 cs "$TGT" "SELECT current_val FROM db_serial WHERE name='rt_seq'")" "$serial_cur"
# ... but its START WITH does not survive, because unloaddb writes the current
# value there. importdb replays the dump faithfully; the loss is upstream, so
# start_val is excluded from the fingerprint and pinned to the dump text here.
assert_grep "unloaddb wrote the serial's current value as START WITH" \
  "$DUMP/${SRC}_schema" "START WITH $serial_cur"
assert_eq "imported serial start_val is the source current_val (unloaddb, not importdb)" \
  "$(q1 cs "$TGT" "SELECT start_val FROM db_serial WHERE name='rt_seq'")" "$serial_cur"
assert_eq "trigger defined" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_trigger WHERE trigger_name='trg_dept'")" 1
# the trigger is defined last, after the data: it must not have fired on the
# bulk-loaded rows, which for a BEFORE INSERT ... PRINT trigger would mean the
# load printed for every row.
assert_no_grep "trigger did not fire during the load" "$WORK/import.log" "dept insert"

case_exit
