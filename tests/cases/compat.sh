#!/bin/bash
# compat -- the pre-11.5 CALL-target rewrite, and what it must not touch.
#
# importdb rewrites three `CALL ... ON CLASS <view>` targets in a pre-11.5
# schema file before it is parsed, because 11.5 made those views distinct from
# the classes that carry the methods (see rewrite_pre115_call_targets () in
# src/import_define.cpp). That is a TEXT transform on SQL, so it has one real
# risk: acting on a name that is not a statement's ON CLASS target.
#
# This case is the guard on that risk. Its dump is hand-written and checked in,
# so the case needs no second engine and runs on every host -- unlike
# crossversion, which needs a real old install and skips without one.
#
# The fixture puts every string the scanner looks for somewhere it must NOT act:
# inside a DEFAULT's string literal, after a `--` marker inside that literal,
# inside a `/* */` block comment, and inside a loaded row's data. It also carries the two `find_user` statements that
# are exempt and the one `change_serial_owner` that is not, so the reported
# rewrite count -- exactly 1 -- is the assertion.
set -uo pipefail
export CASE_NAME=compat
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

TGT=it_cp_tgt
DUMP="$WORK/dump"
LITERAL='call [x] on class [db_serial] -- and on class [db_user]'
ROWTEXT='a row whose text says on class [db_serial] and on class [db_user]'

mkdir -p "$DUMP" || die "cannot create $DUMP"
cp "$IT_FIXTURES_DIR/dumps/pre115_schema"  "$DUMP/cp_schema"  || die "fixture dump missing"
cp "$IT_FIXTURES_DIR/dumps/pre115_objects" "$DUMP/cp_objects" || die "fixture dump missing"

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"

run_import "$WORK/import.log" -u dba "$TGT" "$DUMP"
assert_rc "a pre-11.5 shaped dump imports clean" "$IT_RC" 0
assert_grep "report verdict is COMPLETE" "$WORK/import.log" "import COMPLETE"

# One rewrite, not three: the two find_user statements are exempt, and neither
# literal counts. A count of 3 means the exemption was lost; more than 3 means
# the scanner is matching inside literals.
assert_grep "exactly one target was rewritten" "$WORK/import.log" \
  "rewrote 1 pre-11.5 catalog target\\(s\\)"

# The rewrite must not have moved anything: it inserts one character on the line
# it changes and never a newline, so a later diagnostic's line number still
# refers to the file on disk.
assert_eq "the schema file itself was not modified" \
  "$(sha256sum "$DUMP/cp_schema" | cut -d' ' -f1)" \
  "$(sha256sum "$IT_FIXTURES_DIR/dumps/pre115_schema" | cut -d' ' -f1)"

# what the rewrite was for
assert_eq "the serial was created and is owned" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_serial WHERE name='cp_seq'")" 1

# what the rewrite must have left alone
assert_eq "a string literal naming db_serial survived verbatim in a DEFAULT" \
  "$(sql_cs "$TGT" "SELECT default_value FROM db_attribute WHERE class_name='cp_t' AND attr_name='note'" | sed -n '1{s/^ *//;s/ *$//;p}')" \
  "$LITERAL"
assert_eq "and verbatim in a loaded row" \
  "$(sql_cs "$TGT" "SELECT note FROM cp_t WHERE id=1" | sed -n '1{s/^ *//;s/ *$//;p}')" \
  "$ROWTEXT"
assert_eq "no row picked up an underscore-prefixed catalog name" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM cp_t WHERE note LIKE '%!_db!_serial%' ESCAPE '!' OR note LIKE '%!_db!_user%' ESCAPE '!'")" 0
assert_eq "both rows loaded" "$(q1 cs "$TGT" "SELECT count(*) FROM cp_t")" 2

# and a dump with no pre-11.5 target must not report a rewrite at all
sed -i 's/on class \[db_serial\]/on class [_db_serial]/' "$DUMP/cp_schema"
db_drop "$TGT"
db_create "$TGT" || die "cannot re-create $TGT"
db_start "$TGT" || die "cannot re-start $TGT"
rm -f "$DUMP/importdb.manifest"
run_import "$WORK/already.log" -u dba "$TGT" "$DUMP"
assert_rc "a dump already naming the underlying class imports clean too" "$IT_RC" 0
assert_no_grep "and reports no rewrite" "$WORK/already.log" "pre-11.5 catalog target"

case_exit
