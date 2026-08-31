#!/bin/bash
# corrupt -- what happens when the dump itself is damaged?
#
# OFFICE-656 2-1: the release suite tests the happy path, so a damaged dump is
# untested. This case damages one in six ways and pins what importdb actually
# does with each. Five of the six are caught; two of the six are NOT, and this
# case asserts that too, because a characterization test that quietly asserts
# the behaviour we wish for would hide the finding instead of recording it.
#
# The two that are not caught share one cause and it is not importdb's to fix:
# an unloaddb dump carries no statement of what it contains -- no per-class row
# count, no roster of the object files that should exist. A dump missing content
# is therefore indistinguishable from a dump OF less content, to loaddb and to
# importdb alike. Closing it needs a row count in the dump, which is upstream.
#
# Each scenario starts from a pristine copy of one good dump, so the damage is
# the only difference.
set -uo pipefail
export CASE_NAME=corrupt
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_co_src
GOOD="$WORK/good"

db_create "$SRC" || die "cannot create $SRC"
fixture_apply "$SRC" legacy || die "fixture DDL failed (see $WORK/legacy.ddl.log)"
fixture_rows "$SRC" legacy || die "fixture rows failed (see $WORK/legacy.rows.log)"
unload_dump "$SRC" "$GOOD" --datafile-per-class || die "unloaddb failed"

SRC_PARENT=$(q1 sa "$SRC" "SELECT count(*) FROM lg_parent")
SRC_CHILD=$(q1 sa "$SRC" "SELECT count(*) FROM lg_child")
CHILD_OBJ="${SRC}_dba.lg_child_objects"
assert_file "the per-class dump has its own child object file" "$GOOD/$CHILD_OBJ"

# Each scenario gets a fresh copy of the good dump and a fresh target.
n=0
prepare () { # tag -> sets DUMP and TGT
  n=$((n + 1))
  DUMP="$WORK/d$n"
  TGT="it_co_t$n"
  rm -rf "$DUMP"
  cp -r "$GOOD" "$DUMP" || die "cannot copy the good dump"
  db_create "$TGT" || die "cannot create $TGT"
  db_start "$TGT" || die "cannot start $TGT"
}

# ------------------------------------------- 1. a value of the wrong data type

prepare
# the child's first data line is `<cid> <pid> <amount>`; make pid a string
sed -i "2s/.*/1 'not-an-integer' 100.75/" "$DUMP/$CHILD_OBJ"
run_import "$WORK/type.log" -u dba "$TGT" "$DUMP"
assert_nonzero_rc "a data type mismatch is refused, not coerced" "$IT_RC"
assert_grep "the failing object file is named" "$WORK/type.log" \
  "loading object data from '$CHILD_OBJ' failed"
assert_grep "and the loader's own count of failed objects is surfaced" "$WORK/type.log" \
  "object\\(s\\) failed"
assert_no_grep "the run does not claim to be complete" "$WORK/type.log" "import COMPLETE"
assert_eq "no child row was committed" "$(q1 cs "$TGT" "SELECT count(*) FROM lg_child")" 0

# ------------------------------------------------ 2. an object file of garbage

prepare
head -c 4096 /dev/urandom > "$DUMP/$CHILD_OBJ"
run_import "$WORK/garbage.log" -u dba "$TGT" "$DUMP"
assert_nonzero_rc "an object file of random bytes is refused" "$IT_RC"
assert_grep "the failing object file is named" "$WORK/garbage.log" \
  "loading object data from '$CHILD_OBJ' failed"
assert_no_grep "the run does not claim to be complete" "$WORK/garbage.log" "import COMPLETE"

# --------------------------------------------- 3. a schema file that will not parse

prepare
awk 'NR == 8 { print "ALTER CLASS [dba].[lg_parent] ADD ATTRIBUTE [broken] NOT A TYPE;"; next } { print }' \
  "$DUMP/${SRC}_schema" > "$DUMP/schema.tmp" && mv "$DUMP/schema.tmp" "$DUMP/${SRC}_schema"
run_import "$WORK/schema.log" -u dba "$TGT" "$DUMP"
assert_nonzero_rc "a schema file that will not parse is refused" "$IT_RC"
assert_grep "the file and the line are named" "$WORK/schema.log" \
  "definition failed in .*${SRC}_schema at line 8"
# the definition phase is all-or-nothing, so a parse error anywhere leaves the
# target exactly as empty as it was
assert_eq "and the target is left completely undefined" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE is_system_class='NO'")" 0
# The manifest IS written -- at discovery, before the definition is attempted --
# and that is the point of it: it records what was rostered. What must be true is
# that the failed definition left nothing durable behind it, so the manifest
# still stops at 'discovered' and a re-run starts from the definition.
assert_file "the manifest records what was rostered" "$DUMP/importdb.manifest"
assert_eq "and stops at discovery, so the failed definition left nothing durable" \
  "$(manifest_phase "$DUMP")" "discovered"

# ------------------------------------------------------ 4. a corrupt manifest

prepare
printf '# CUBRID importdb manifest (format v3)\n# truncated before any phase was recorded\n' \
  > "$DUMP/importdb.manifest"
before=$(sha256sum "$DUMP/importdb.manifest" | cut -d' ' -f1)
run_import "$WORK/manifest.log" -u dba "$TGT" "$DUMP"
assert_nonzero_rc "a manifest that records no phase is refused" "$IT_RC"
assert_grep "the diagnostic says why it cannot be resumed" "$WORK/manifest.log" \
  "is present but records no phase"
assert_eq "and the file was left untouched, not overwritten" \
  "$(sha256sum "$DUMP/importdb.manifest" | cut -d' ' -f1)" "$before"
assert_eq "the target was not defined either" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM db_class WHERE is_system_class='NO'")" 0

# ------------------------- 5. a truncated object file -- NOT caught (see below)

prepare
sz=$(stat -c%s "$DUMP/$CHILD_OBJ")
head -c $((sz / 2)) "$DUMP/$CHILD_OBJ" > "$DUMP/obj.tmp" && mv "$DUMP/obj.tmp" "$DUMP/$CHILD_OBJ"
run_import "$WORK/trunc.log" -u dba "$TGT" "$DUMP"
got=$(q1 cs "$TGT" "SELECT count(*) FROM lg_child")
assert_rc "a truncated object file is NOT detected -- the run succeeds" "$IT_RC" 0
assert_grep "and reports COMPLETE" "$WORK/trunc.log" "import COMPLETE"
if [ "$got" -lt "$SRC_CHILD" ]; then
  pass "rows were silently lost ($got of $SRC_CHILD), which is the finding"
else
  fail "expected fewer than $SRC_CHILD child rows from a halved object file, got $got"
fi
assert_eq "the parent class, whose file was intact, is complete" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM lg_parent")" "$SRC_PARENT"
note "the dump carries no per-class row count, so nothing downstream can tell a
      truncated object file from a shorter one. Closing this needs the count in
      the dump -- an unloaddb change, not an importdb one."

# --------------------- 6. a missing per-class object file -- NOT caught either

prepare
rm -f "$DUMP/$CHILD_OBJ"
run_import "$WORK/missing.log" -u dba "$TGT" "$DUMP"
assert_rc "a deleted per-class object file is NOT detected either" "$IT_RC" 0
assert_grep "and reports COMPLETE" "$WORK/missing.log" "import COMPLETE"
assert_eq "the class it held is silently empty" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM lg_child")" 0
assert_eq "its schema, constraints and the other class are all there" \
  "$(q1 cs "$TGT" "SELECT count(*) FROM lg_parent")" "$SRC_PARENT"
note "same cause as 5: a dump has no roster of the object files it should
      contain, so a dump missing one is a valid dump of fewer classes. Discovery
      rosters what is present, by design -- a per-class dump legitimately omits
      an empty class's file."

case_exit
