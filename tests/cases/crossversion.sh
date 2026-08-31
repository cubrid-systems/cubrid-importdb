#!/bin/bash
# crossversion -- does a dump written by an OLDER engine import correctly?
#
# This is the OFFICE-656 axis: a customer upgrades, so the dump is written by
# the version they are leaving and read by the version they are arriving at.
# Every other case in this suite unloads and imports with the same install, so
# none of them can see a cross-version break at all.
#
# The lane needs a second CUBRID install and does nothing without one:
#
#     IT_SRC_CUBRID=/path/to/10.2/install tests/run_tests.sh crossversion
#
# The source engine is used standalone only -- createdb, `csql -S`, `unloaddb -S`
# -- so the two installs need no port separation and never see each other.
#
# The floor is 10.2 deliberately. The two hard 9.x -> 10.x breaks are below it
# and are not this tool's problem: password hashing changed from SHA1 to SHA2 in
# 10.0 (CBRD-20659) and the reuse_oid default changed in 10.0 (CBRD-23708), both
# of which need a migration step before any loader runs.
#
# THREE comparisons, and none of them is loosened for being cross-version:
#
#   data     the 10.2 source vs the 11.5 target, per class, by row count and by
#            an order-independent content checksum. Byte-identical.
#   catalog  the target built from the 10.2 dump vs a target built from an 11.5
#            dump of the SAME fixture, through the same fingerprint the
#            roundtrip case uses. Byte-identical -- which is the real question an
#            upgrade asks: does an old dump land where a new one lands?
#   auth     the users and their grants on the dump's own classes, same two
#            targets. Byte-identical.
#
# The reference arm is what makes the catalog half possible: both sides are
# 11.5 catalogs, so the strict fingerprint applies unchanged and no
# version-neutral subset has to be invented.
set -uo pipefail
export CASE_NAME=crossversion
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

if ! src_is_foreign; then
  skip "no source engine: set IT_SRC_CUBRID to an older CUBRID install (10.2 or
      newer) to run this lane. Without it there is nothing to compare -- the
      rest of the suite already covers same-version import."
  case_exit
fi

if ! src_engine_init; then
  unresolved=$(src_engine_unresolved)
  if [ -n "$unresolved" ]; then
    skip "source engine $(src_engine_dir) cannot run here: unresolved $unresolved.
      A published 10.x binary links the ncurses 5 sonames; the harness links
      whatever major version is installed under those names, and this host has
      none to link."
  else
    skip "source engine $(src_engine_dir) is not usable (no bin/cubrid, or it
      would not run)."
  fi
  case_exit
fi

note "source engine: $(src_engine_dir) ($(src_engine_version))"
note "target engine: $CUBRID"

OLD=it_xv_old            # the old-version source database
NEW=it_xv_new            # the same fixture, built natively on the target engine
XTGT=it_xv_from_old      # target imported from the OLD dump
RTGT=it_xv_from_new      # target imported from the NEW dump  (the reference)
XDUMP="$WORK/dump_old"
RDUMP="$WORK/dump_new"

# ------------------------------------------------- the old-version source side

src_db_create "$OLD" || die "cannot create $OLD with the source engine"
for fx in types legacy; do
  src_fixture_apply "$OLD" "$fx" || die "source fixture DDL '$fx' failed (see $WORK/src.$fx.ddl.log)"
done
for fx in types legacy; do
  src_fixture_rows "$OLD" "$fx" || die "source fixture rows '$fx' failed (see $WORK/src.$fx.rows.log)"
done

src_data_fp "$OLD" "$WORK/old.data"
src_unload_dump "$OLD" "$XDUMP" || die "source-engine unloaddb failed"

OLD_SCHEMA="$XDUMP/${OLD}_schema"
assert_file "the old engine wrote a schema file" "$OLD_SCHEMA"

# The shape that makes this dump *old*, pinned so the case still means something
# if a future source engine stops writing it. A pre-11.5 unloaddb names the
# catalog VIEWS as CALL targets; 11.5 made those views distinct from the classes
# that carry the methods.
assert_grep "the old dump names a pre-11.5 catalog view as a CALL target" \
  "$OLD_SCHEMA" "on class \\[db_serial\\]"
assert_grep "and creates its classes unqualified, then moves the owner" \
  "$OLD_SCHEMA" "call \\[change_owner\\]"

# ------------------------------------------------------ the same-version source

db_create "$NEW" || die "cannot create $NEW"
for fx in types legacy; do
  fixture_apply "$NEW" "$fx" || die "fixture DDL '$fx' failed (see $WORK/$fx.ddl.log)"
done
for fx in types legacy; do
  fixture_rows "$NEW" "$fx" || die "fixture rows '$fx' failed (see $WORK/$fx.rows.log)"
done
unload_dump "$NEW" "$RDUMP" || die "unloaddb failed"

assert_no_grep "the current engine's own dump has no pre-11.5 CALL target" \
  "$RDUMP/${NEW}_schema" "on class \\[db_serial\\]"

# ----------------------------------------------------------------- the imports

db_create "$XTGT" || die "cannot create $XTGT"
db_start "$XTGT" || die "cannot start $XTGT"
run_import "$WORK/old.import.log" -u dba "$XTGT" "$XDUMP"
assert_rc "the old-version dump imports clean" "$IT_RC" 0
assert_grep "report verdict is COMPLETE" "$WORK/old.import.log" "import COMPLETE"

# The compat rewrite must have happened, and exactly once: the dump carries one
# `on class [db_serial]` plus two `find_user ... on class [db_user]`, and
# find_user is the one method the db_user view kept. A rewrite count of 3 would
# mean the exemption was lost.
assert_grep "the pre-11.5 CALL target was rewritten" "$WORK/old.import.log" \
  "rewrote 1 pre-11.5 catalog target\\(s\\)"

db_create "$RTGT" || die "cannot create $RTGT"
db_start "$RTGT" || die "cannot start $RTGT"
run_import "$WORK/new.import.log" -u dba "$RTGT" "$RDUMP"
assert_rc "the same-version dump imports clean (the reference arm)" "$IT_RC" 0
assert_no_grep "and needed no compat rewrite" "$WORK/new.import.log" "pre-11.5 catalog target"

# ------------------------------------------------------------- the comparisons

data_fp cs "$XTGT" "$WORK/xtgt.data"
row_counts cs "$XTGT" "$WORK/xtgt.rowcounts"
catalog_fp cs "$XTGT" "$WORK/xtgt.catalog"
catalog_fp cs "$RTGT" "$WORK/rtgt.catalog"
auth_fp "$XTGT" "$WORK/xtgt.auth"
auth_fp "$RTGT" "$WORK/rtgt.auth"

for side in old xtgt; do
  awk '{ print $1, $2 }' "$WORK/$side.data" > "$WORK/$side.counts"
  awk '{ print $1, $3 }' "$WORK/$side.data" > "$WORK/$side.sums"
done

assert_same "row count per class survives the version gap" \
  "$WORK/old.counts" "$WORK/xtgt.counts"
assert_same "content checksum per class survives the version gap" \
  "$WORK/old.sums" "$WORK/xtgt.sums"
assert_same "and the counts agree when the server is asked directly" \
  "$WORK/xtgt.counts" "$WORK/xtgt.rowcounts"
assert_same "the old dump lands on the same catalog as a current dump" \
  "$WORK/rtgt.catalog" "$WORK/xtgt.catalog"
assert_same "users and grants land identically too" \
  "$WORK/rtgt.auth" "$WORK/xtgt.auth"

# ------------------------------------- the families called out by name, so a
#                                       diff-only failure names what was lost

assert_eq "ENUM value carried" \
  "$(q1 cs "$XTGT" "SELECT e FROM ty_enum WHERE id=2")" "large"
assert_eq "JSON object carried" \
  "$(q1 cs "$XTGT" "SELECT json_extract(j,'\$.c.d') FROM ty_json WHERE id=1")" "\"e\""
# CBRD-26282: a value past unloaddb's internal buffer came back corrupted.
assert_eq "the 978 KB VARCHAR is intact to the byte" \
  "$(q1 cs "$XTGT" "SELECT length(s) FROM ty_wide WHERE id=1")" "978670"
assert_eq "collection columns carried" \
  "$(q1 cs "$XTGT" "SELECT count(*) FROM ty_coll WHERE s={1,2,3}")" 1
assert_eq "BLOB/CLOB carried" \
  "$(q1 cs "$XTGT" "SELECT length(clob_to_char(cl)) FROM ty_lob WHERE id=1")" "17"
assert_eq "HASH partitions all populated" \
  "$(q1 cs "$XTGT" "SELECT count(*) FROM db_partition WHERE class_name='ty_hash'")" 3
assert_eq "LIST partitions all populated" \
  "$(q1 cs "$XTGT" "SELECT count(*) FROM ty_list")" 4
assert_eq "the foreign key was defined against the loaded rows" \
  "$(q1 cs "$XTGT" "SELECT count(*) FROM db_index WHERE class_name='lg_child' AND is_foreign_key='YES'")" 1
assert_eq "the view came back and answers" \
  "$(q1 cs "$XTGT" "SELECT count(*) FROM lg_v_child")" 40
assert_eq "the serial kept its advanced current value" \
  "$(q1 cs "$XTGT" "SELECT current_val FROM db_serial WHERE name='lg_seq'")" \
  "$(src_sql "$OLD" "SELECT current_val FROM db_serial WHERE name='lg_seq'" | tr -d ' \t\n')"
assert_eq "the trigger came back" \
  "$(q1 cs "$XTGT" "SELECT count(*) FROM db_trigger WHERE trigger_name='lg_trg'")" 1
assert_no_grep "and did not fire during the load" "$WORK/old.import.log" "lg_parent insert"

# ------------------------------------------------------------- can they log in?
#
# OFFICE-656 1-1 verifies "사용자 인증, 테이블 접근" -- authentication and table
# access -- and it is right to: an account that cannot log in after the upgrade
# is the first thing an operator meets, and neither the catalog fingerprint nor
# the grant set above can see it. The password comes across in the schema file as
# `call [set_password_encoded_sha1](..) on [auser]`, which is a separate
# statement from the `call [add_user]('X', '')` above it -- reading only the
# add_user line is what makes people believe passwords are lost.
assert_eq "the password set on the OLD engine still authenticates" \
  "$(sql_cs_as "$XTGT" lg_writer lgpw "SELECT count(*) FROM dba.lg_child" | tr -d ' \t\n')" 41
sql_cs_as "$XTGT" lg_writer definitely-not-the-password "SELECT 1 FROM db_root" \
  > "$WORK/badpw.log" 2>&1
assert_grep "and a wrong password is still refused" "$WORK/badpw.log" \
  "Incorrect or missing password"
assert_eq "a user created without a password needs none, and sees its grant" \
  "$(sql_cs_as "$XTGT" lg_reader "" "SELECT count(*) FROM dba.lg_parent" | tr -d ' \t\n')" 12

case_exit
