#!/bin/bash
# dupname -- the same constraint name on more than one class.
#
# Three classes each carry a PRIMARY KEY named pk1 and two of them a UNIQUE named
# u1, which CUBRID allows because index names are per class. The import must put
# every one of them back AND record which class each belongs to: the catalog
# fingerprint proves the placement, the manifest's [rebuild] section proves the
# record, and the record is what a repair would be driven from.
set -uo pipefail
export CASE_NAME=dupname
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_dup_src
TGT=it_dup_tgt
DUMP="$WORK/dump"

db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" dupname || die "fixture DDL failed"
sql_sa "$SRC" "INSERT INTO dn_a VALUES (1,'a1'),(2,'a2');
               INSERT INTO dn_b VALUES (1,'b1'),(2,'b2');
               INSERT INTO dn_c VALUES (1),(2);" > "$WORK/rows.log" 2>&1

assert_eq "the source reuses one PK name across three classes" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM db_index WHERE index_name='pk1' AND is_primary_key='YES'")" 3
assert_eq "and one UNIQUE name across two" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM db_index WHERE index_name='u1' AND is_unique='YES' AND is_primary_key='NO'")" 2

catalog_fp sa "$SRC" "$WORK/src.catalog"
data_fp sa "$SRC" "$WORK/src.data"
# the DEFAULT single-file layout on purpose: its name filter is what resolved the
# repeated names, so a per-class dump would not exercise the isolation at all
unload_dump "$SRC" "$DUMP" || die "unloaddb failed"

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"

run_import "$WORK/import.log" -u dba "$TGT" "$DUMP"
assert_rc "the import exits clean" "$IT_RC" 0
assert_grep "the report verdict is COMPLETE" "$WORK/import.log" "import COMPLETE"
assert_grep "all five stripped constraints came back" "$WORK/import.log" \
  "rebuilt 5 constraint\\(s\\)"

# the record, not the count: each class against its own constraint name, once
manifest_ok () { # kind class name
  awk -F'\t' -v k="ok: $1" -v c="$2" -v n="$3" \
    '$1==k && $2==c && $3==n { hit++ } END { print hit + 0 }' "$DUMP/importdb.manifest"
}
for cls in dn_a dn_b dn_c; do
  assert_eq "the rebuild record names $cls's own pk1, once" "$(manifest_ok pk "$cls" pk1)" 1
done
for cls in dn_a dn_b; do
  assert_eq "the rebuild record names $cls's own u1, once" "$(manifest_ok unique "$cls" u1)" 1
done

catalog_fp cs "$TGT" "$WORK/tgt.catalog"
data_fp cs "$TGT" "$WORK/tgt.data"
assert_same "catalog fingerprint is identical" "$WORK/src.catalog" "$WORK/tgt.catalog"
assert_same "row counts and checksums are identical" "$WORK/src.data" "$WORK/tgt.data"

case_exit
