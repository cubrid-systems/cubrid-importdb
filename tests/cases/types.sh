#!/bin/bash
# types -- does every column type survive the round trip?
#
# OFFICE-656 2-3: the release suite covers a limited set of column types, so a
# family that does not survive a reload has nowhere to show up. The roundtrip
# case here had the same hole -- it carried integer, character, the date/time
# four, exact numeric and float, and nothing else.
#
# This case adds the families that were missing, one table each so a failure
# names the family: ENUM, JSON, BLOB/CLOB, BIT, SET/MULTISET/SEQUENCE, the four
# zoned date/time types, MONETARY, NCHAR, two collations in one row, non-ASCII
# text, HASH and LIST partitioning, a filtered index, a function index, and one
# VARCHAR value larger than unloaddb's internal buffer -- the shape that came
# back corrupted in CBRD-26282.
#
# The comparison is the same as roundtrip's and just as strict: the catalog
# fingerprint must be byte-identical, and every class's row count and
# order-independent content checksum must match the source.
set -uo pipefail
export CASE_NAME=types
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_ty_src
TGT=it_ty_tgt
DUMP="$WORK/dump"

db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" types || die "fixture DDL failed"
fixture_rows "$SRC" types || die "fixture rows failed"

catalog_fp sa "$SRC" "$WORK/src.catalog"
data_fp sa "$SRC" "$WORK/src.data"
row_counts sa "$SRC" "$WORK/src.rowcounts"
unload_dump "$SRC" "$DUMP" || die "unloaddb failed"

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"

run_import "$WORK/import.log" -u dba "$TGT" "$DUMP"
assert_rc "import exits clean" "$IT_RC" 0
assert_grep "report verdict is COMPLETE" "$WORK/import.log" "import COMPLETE"

catalog_fp cs "$TGT" "$WORK/tgt.catalog"
data_fp cs "$TGT" "$WORK/tgt.data"
row_counts cs "$TGT" "$WORK/tgt.rowcounts"
for side in src tgt; do
  awk '{ print $1, $2 }' "$WORK/$side.data" > "$WORK/$side.counts"
  awk '{ print $1, $3 }' "$WORK/$side.data" > "$WORK/$side.sums"
done

assert_same "catalog fingerprint is identical" "$WORK/src.catalog" "$WORK/tgt.catalog"
assert_same "row count per class is identical" "$WORK/src.counts" "$WORK/tgt.counts"
assert_same "and identical when counted by the server" \
  "$WORK/src.rowcounts" "$WORK/tgt.rowcounts"
assert_same "content checksum per class is identical" "$WORK/src.sums" "$WORK/tgt.sums"

# --------------------------- the families by name, so a diff does not hide one

assert_eq "ENUM keeps its value" \
  "$(q1 cs "$TGT" "SELECT e FROM ty_enum WHERE id=1")" "small"
assert_eq "ENUM keeps its domain, not just the string" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_attribute WHERE class_name='ty_enum' AND attr_name='e' AND data_type='ENUM'")" 1
assert_eq "JSON keeps its structure" \
  "$(q1 cs "$TGT" "SELECT json_extract(j,'\$.c.d') FROM ty_json WHERE id=1")" "\"e\""
assert_eq "an empty JSON array is not a NULL" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_json WHERE id=2 AND j IS NOT NULL")" 1
# q1 strips whitespace, so the CLOB's own spaces go with it -- the point here is
# that the characters arrived, and the checksum above already covers the exact value.
assert_eq "CLOB content survives" \
  "$(q1 cs "$TGT" "SELECT clob_to_char(cl) FROM ty_lob WHERE id=1")" "clobcontenthere"
assert_eq "BLOB content survives, bit for bit" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_lob WHERE id=1 AND blob_to_bit(bl) = B'11110000'")" 1
assert_eq "BIT keeps its bits" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_bit WHERE id=1 AND b = B'1010101010101010'")" 1
assert_eq "SET keeps its members" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_coll WHERE id=1 AND s={1,2,3}")" 1
assert_eq "an empty collection is still a collection" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_coll WHERE id=2 AND s IS NOT NULL")" 1
assert_eq "SEQUENCE keeps its order (a SET would have sorted it)" \
  "$(q1 cs "$TGT" "SELECT q FROM ty_coll WHERE id=1")" "{3,1,2}"
assert_eq "the zoned timestamp keeps its zone" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_time WHERE id=1 AND tstz = TIMESTAMPTZ'2020-02-29 23:59:59 Asia/Seoul'")" 1
assert_eq "MONETARY survives" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_num WHERE id=1 AND mon = 1234.56")" 1
assert_eq "NUMERIC(38,0) keeps all 38 digits" \
  "$(q1 cs "$TGT" "SELECT n_big FROM ty_num WHERE id=1")" "12345678901234567890123456789012345678"
assert_eq "BIGINT keeps its extremes" \
  "$(q1 cs "$TGT" "SELECT bi FROM ty_num WHERE id=1")" "9223372036854775807"
assert_eq "non-ASCII text survives" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_str WHERE ko = '한글 문자열 테스트'")" 1
assert_eq "the per-column collation survives" \
  "$(q1 cs "$TGT" "SELECT collation FROM db_attribute WHERE class_name='ty_str' AND attr_name='vi'")" "iso88591_bin"
# CBRD-26282: a 978 KB VARCHAR came back corrupted through unloaddb/loaddb.
assert_eq "the 978 KB VARCHAR is intact to the byte" \
  "$(q1 cs "$TGT" "SELECT length(s) FROM ty_wide WHERE id=1")" "978670"
assert_eq "HASH partitioning survives with all three partitions" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_partition WHERE class_name='ty_hash' AND partition_type='HASH'")" 3
assert_eq "and every HASH partition's rows landed" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM ty_hash")" 6
assert_eq "LIST partitioning survives" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_partition WHERE class_name='ty_list' AND partition_type='LIST'")" 2
assert_eq "the filtered index kept its predicate" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_index WHERE index_name='i_ty_filtered' AND filter_expression IS NOT NULL")" 1
assert_eq "the function index kept its function" \
  "$(q1 cs "$TGT" "SELECT have_function FROM db_index WHERE index_name='i_ty_func'")" "YES"
assert_eq "the table and column COMMENTs survive" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_attribute WHERE class_name='ty_cmt' AND attr_name='id' AND comment='the id'")" 1
# The AUTO_INCREMENT serial is deliberately checked as an invariant, not as an
# equality. Two reasons, both upstream: the db_serial VIEW omits AI serials
# entirely (which is why importdb reads _db_serial at all), and current_val
# reflects a cached allocation whose size differs between a standalone reader and
# a client-server one -- so source and target legitimately report different
# numbers. What must hold is the property an operator depends on after a
# migration: the serial cannot hand out an id that already exists.
assert_eq "the AUTO_INCREMENT serial came back attached to its column" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM _db_serial WHERE class_name='ty_ai' AND attr_name='id'")" 1
ai_cur=$(q1 cs "$TGT" "SELECT current_val FROM _db_serial WHERE class_name='ty_ai'")
ai_max=$(q1 cs "$TGT" "SELECT max(id) FROM ty_ai")
if [ "${ai_cur:-0}" -ge "${ai_max:-0}" ]; then
  pass "the serial cannot collide with a loaded row (current $ai_cur >= max id $ai_max)"
else
  fail "the AUTO_INCREMENT serial is behind the data: current $ai_cur < max id $ai_max"
fi

case_exit
