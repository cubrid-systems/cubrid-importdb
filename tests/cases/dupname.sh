#!/bin/bash
# dupname -- the same constraint name on more than one class.
#
# What the Rebuild and FK phases have to read out of the dump's text is a
# constraint's IDENTITY, and both halves of it repeat: five classes each carry a
# PRIMARY KEY named pk1, two a UNIQUE named u1, two a FOREIGN KEY named fk1, and
# one is named "dn add e" -- all legal, because CUBRID constraint names are unique
# per class rather than per database and a delimited class name may contain the
# ADD keyword the matcher looks for. Every constraint must be back at the end AND
# recorded against the class it belongs to: the catalog fingerprint proves the
# placement, the manifest's [rebuild] section proves the record, and the record is
# what a repair would be driven from. Run against both schema layouts, because
# each resolves the same identity out of a differently shaped file.
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
               INSERT INTO dn_c VALUES (1),(2);
               INSERT INTO dn_d VALUES (1),(2);
               INSERT INTO \"dn add e\" VALUES (1),(2);" > "$WORK/rows.log" 2>&1
assert_eq "the fixture rows really loaded" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM dn_a")" 2

assert_eq "the source reuses one PK name across five classes" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM db_index WHERE index_name='pk1' AND is_primary_key='YES'")" 5
assert_eq "and one of those classes has the ADD keyword in its name" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM db_class WHERE class_name='dn add e'")" 1
assert_eq "one UNIQUE name across two" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM db_index WHERE index_name='u1' AND is_unique='YES' AND is_primary_key='NO'")" 2
assert_eq "and one FK name across two" \
  "$(q1 sa "$SRC" "SELECT count(*) FROM db_index WHERE index_name='fk1' AND is_foreign_key='YES'")" 2

catalog_fp sa "$SRC" "$WORK/src.catalog"
data_fp sa "$SRC" "$WORK/src.data"
# DEFAULT layout: one interleaved schema file, out of which the class-and-name key
# has to pick the PK/UK statements. The SPLIT arm at the bottom is the same key
# resolving the same identities in the separated _schema_pk / _schema_uk files.
unload_dump "$SRC" "$DUMP" || die "unloaddb failed"

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"

run_import "$WORK/import.log" -u dba "$TGT" "$DUMP"
assert_rc "the import exits clean" "$IT_RC" 0
assert_grep "the report verdict is COMPLETE" "$WORK/import.log" "import COMPLETE"
assert_grep "all seven stripped PK/UNIQUE constraints came back" "$WORK/import.log" \
  "rebuilt 7 constraint\\(s\\) and built 0 index\\(es\\)"
# the FK phase keyed its edges by name too, and there a collapsed entry meant the
# second statement was dropped as already processed -- one FK simply never defined
assert_grep "both FK edges were accepted, not one" "$WORK/import.log" \
  "2 edge\\(s\\) clean"
assert_grep "and both were defined" "$WORK/import.log" "defined 2 FK\\(s\\)"
assert_eq "both children carry their own fk1 in the target" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_index WHERE index_name='fk1' AND is_foreign_key='YES'")" 2

# the record, not the count: each class against its own constraint name, once
manifest_ok () { # kind class name
  awk -F'\t' -v k="ok: $1" -v c="$2" -v n="$3" \
    '$1==k && $2==c && $3==n { hit++ } END { print hit + 0 }' "$DUMP/importdb.manifest"
}
for cls in dn_a dn_b dn_c dn_d "dn add e"; do
  assert_eq "the rebuild record names $cls's own pk1, once" "$(manifest_ok pk "$cls" pk1)" 1
done
for cls in dn_a dn_b; do
  assert_eq "the rebuild record names $cls's own u1, once" "$(manifest_ok unique "$cls" u1)" 1
done

catalog_fp cs "$TGT" "$WORK/tgt.catalog"
data_fp cs "$TGT" "$WORK/tgt.data"
assert_same "catalog fingerprint is identical" "$WORK/src.catalog" "$WORK/tgt.catalog"
assert_same "row counts and checksums are identical" "$WORK/src.data" "$WORK/tgt.data"

# ------------------------------------------------ the same identities, SPLIT layout
SPLIT_DUMP="$WORK/split"
SPLIT_TGT=it_dup_tgt2
unload_dump "$SRC" "$SPLIT_DUMP" --split-schema-files || die "split unloaddb failed"
assert_eq "the split dump really separated its PK/UK files" \
  "$(find "$SPLIT_DUMP" -name '*_schema_pk' -o -name '*_schema_uk' | wc -l | tr -d ' ')" 2

db_create "$SPLIT_TGT" || die "cannot create $SPLIT_TGT"
db_start "$SPLIT_TGT" || die "cannot start $SPLIT_TGT"
run_import "$WORK/split.log" -u dba "$SPLIT_TGT" "$SPLIT_DUMP"
assert_rc "the split-layout import exits clean" "$IT_RC" 0
assert_grep "it was read as a split dump" "$WORK/split.log" "rostered split dump"
assert_grep "and rebuilt the same seven" "$WORK/split.log" \
  "rebuilt 7 constraint\\(s\\) and built 0 index\\(es\\)"
assert_grep "and defined both FKs" "$WORK/split.log" "defined 2 FK\\(s\\)"

catalog_fp cs "$SPLIT_TGT" "$WORK/split.catalog"
assert_same "the split-layout catalog matches the source too" \
  "$WORK/src.catalog" "$WORK/split.catalog"

case_exit
