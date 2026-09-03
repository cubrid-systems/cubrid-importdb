#!/bin/bash
# refusals -- the things importdb must decline, with a named diagnostic.
#
# A refusal is a feature: every one of these is a case where importing anyway
# would leave the operator worse off than being stopped. Each assertion pairs a
# non-zero exit with the specific message, so a refusal that starts happening
# for a different reason still fails the test.
#
# The HA refusal is not exercised here -- see the note at the bottom.
set -uo pipefail
export CASE_NAME=refusals
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_ref_src
TGT=it_ref_tgt

# ------------------------------------------- refusals that need no database

run_import "$WORK/usage.log"
assert_nonzero_rc "no arguments is refused" "$IT_RC"
assert_grep "usage is printed" "$WORK/usage.log" "usage: .*importdb \\[OPTION\\] database-name dump-dir"

run_import "$WORK/usage1.log" -u dba somedb
assert_nonzero_rc "a missing dump-dir positional is refused" "$IT_RC"
assert_grep "usage is printed again" "$WORK/usage1.log" "usage: .*importdb \\[OPTION\\]"

run_import "$WORK/nodir.log" -u dba somedb "$WORK/no-such-directory"
assert_nonzero_rc "a missing dump directory is refused" "$IT_RC"
assert_grep "the unopenable directory is named" "$WORK/nodir.log" \
  "cannot open dump directory '$WORK/no-such-directory'"

mkdir -p "$WORK/empty_dump"
run_import "$WORK/emptydir.log" -u dba somedb "$WORK/empty_dump"
assert_nonzero_rc "a directory with no dump in it is refused" "$IT_RC"
assert_grep "the diagnostic says what was expected" "$WORK/emptydir.log" \
  "no unloaddb dump found in '$WORK/empty_dump'"

# a schema artifact with no object roster: the prefix resolves, the data does not
mkdir -p "$WORK/noobj_dump"
: > "$WORK/noobj_dump/zz_schema"
run_import "$WORK/noobj.log" -u dba somedb "$WORK/noobj_dump"
assert_nonzero_rc "a dump with no object files is refused" "$IT_RC"
assert_grep "the missing object roster is named" "$WORK/noobj.log" \
  "has no object roster \\(zz_objects"

# a real dump is needed from here on
db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" fkviolation || die "fixture DDL failed"
fixture_rows "$SRC" fkviolation || die "fixture rows failed"
unload_dump "$SRC" "$WORK/dump" --datafile-per-class || die "unloaddb failed"

run_import "$WORK/exctbl.log" -u dba --exceptions-table=t somedb "$WORK/dump"
assert_nonzero_rc "--exceptions-table is refused, not ignored" "$IT_RC"
assert_grep "it is named as reserved" "$WORK/exctbl.log" \
  "'--exceptions-table' is reserved for a future release"

# ------------------------------------------------------------ non-DBA user

db_create "$TGT" || die "cannot create $TGT"
db_start "$TGT" || die "cannot start $TGT"
sql_cs "$TGT" "CREATE USER plainuser PASSWORD 'plainpw'" > "$WORK/mkuser.log" 2>&1
assert_eq "a non-DBA user exists to test with" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_user WHERE name='PLAINUSER'")" 1

cp -r "$WORK/dump" "$WORK/dump_nondba"
run_import "$WORK/nondba.log" -u plainuser -p plainpw "$TGT" "$WORK/dump_nondba"
assert_nonzero_rc "a non-DBA user is refused" "$IT_RC"
assert_grep "the user is named as not in the DBA group" "$WORK/nondba.log" \
  "user 'plainuser' is not a member of the DBA group"
assert_eq "the refused run defined nothing" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE is_system_class='NO'")" 0

# ------------------------------------------------------- non-empty target
#
# Two shapes, one refusal. A class whose name collides with the dump's is the
# obvious one; a class the dump does not define is the dangerous one, because the
# graph is read from the target's catalog and would adopt it as a node.

sql_cs "$TGT" "CREATE TABLE unrelated_t (id INTEGER PRIMARY KEY, payload VARCHAR(5))" \
  > "$WORK/unrelated.log" 2>&1
assert_eq "the target holds a class the dump does not define" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE class_name='unrelated_t'")" 1

cp -r "$WORK/dump" "$WORK/dump_unrelated"
run_import "$WORK/unrelated_import.log" -u dba "$TGT" "$WORK/dump_unrelated"
assert_nonzero_rc "a target holding an unrelated class is refused" "$IT_RC"
assert_grep "the pre-existing class is named" "$WORK/unrelated_import.log" \
  "already holds 1 user class\\(es\\) of its own -- unrelated_t"
assert_eq "its primary key was not dropped" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_index WHERE class_name='unrelated_t' AND is_primary_key='YES'")" 1
assert_eq "and nothing of the dump was defined" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE class_name IN ('fv_p','fv_c1','fv_c2')")" 0

sql_cs "$TGT" "CREATE TABLE fv_p (id INTEGER PRIMARY KEY, other VARCHAR(5))" > "$WORK/collide.log" 2>&1
assert_eq "the target now holds a colliding class too" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE class_name='fv_p'")" 1

cp -r "$WORK/dump" "$WORK/dump_nonempty"
run_import "$WORK/nonempty.log" -u dba "$TGT" "$WORK/dump_nonempty"
assert_nonzero_rc "a target holding a colliding class is refused" "$IT_RC"
assert_grep "every pre-existing class is named, in catalog order" "$WORK/nonempty.log" \
  "already holds 2 user class\\(es\\) of its own -- fv_p, unrelated_t"
assert_eq "the pre-existing class was not replaced" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_attribute WHERE class_name='fv_p' AND attr_name='other'")" 1
assert_eq "and none of the dump's other classes were left behind" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE class_name IN ('fv_c1','fv_c2')")" 0

# ------------------------------------------------------------------ HA target

skip "ha_mode=on target: not exercised. It needs a second host or a docker pair
      to bring up a real HA master/standby, which this harness cannot provision;
      a single non-HA server reports ha_mode=off, and forcing the parameter
      without a standby would assert against a fake. The guard itself is in
      import_db.cpp (HA_DISABLED / --allow-ha) and is covered by the m5
      ha51b-docker fixture referenced from import_define.cpp."

case_exit
