#!/bin/bash
# resume -- SIGKILL the import, run the same command again, converge.
#
# Two kill points, because they are different code paths:
#
#   A. part-way into a phase. The manifest is waited for until it records
#      "stripped" -- meaning the strip is committed and the data phase has
#      begun -- and the process is killed while rows are still being loaded.
#      The resumed run has to empty the tables and reload them, because a
#      killed data phase leaves an unknown number of committed rows.
#   B. at a phase boundary. The manifest is waited for until it records
#      "loaded", which is written only after the data phase committed, and the
#      process is killed there. The resumed run skips the data phase entirely
#      and re-enters the rebuild under its guard.
#
# Both must finish on a second identical invocation, and land on exactly the
# state an uninterrupted import produces. A third run, on a manifest that
# records every phase complete, must be a no-op.
set -uo pipefail
export CASE_NAME=resume
# shellcheck source=../lib/common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"
case_init

SRC=it_res_src
BASE=it_res_base
ROWS=20000

db_create "$SRC" 256M 128M || die "cannot create $SRC"
fixture_apply "$SRC" wide || die "fixture DDL failed"
fixture_rows "$SRC" wide "$ROWS" || die "fixture rows failed"
unload_dump "$SRC" "$WORK/dump" --datafile-per-class || die "unloaddb failed"

# the uninterrupted baseline every resumed run is measured against
db_create "$BASE" 512M 256M || die "cannot create $BASE"
db_start "$BASE" || die "cannot start $BASE"
cp -r "$WORK/dump" "$WORK/dump_base"
run_import "$WORK/base.log" -u dba "$BASE" "$WORK/dump_base"
assert_rc "the uninterrupted baseline import exits clean" "$IT_RC" 0
catalog_fp cs "$BASE" "$WORK/base.catalog"
data_fp cs "$BASE" "$WORK/base.data"
total_rows=$(awk -F'rows=' '{ split($2, a, " "); s += a[1] } END { print s + 0 }' "$WORK/base.data")
note "baseline: $total_rows row(s) across $(wc -l < "$WORK/base.data" | tr -d ' ') class(es)"
db_drop "$BASE"

# kill_at <label> <db> <phase-to-wait-for> <extra-sleep-seconds>
kill_at () {
  local label=$1 tgt=$2 phase=$3 delay=$4
  local dump="$WORK/dump_$label"

  cp -r "$WORK/dump" "$dump"
  db_create "$tgt" 512M 256M || die "cannot create $tgt"
  db_start "$tgt" || die "cannot start $tgt"

  run_import_bg "$WORK/$label.kill.log" -u dba "$tgt" "$dump"
  if ! wait_phase "$dump" "$phase" 2000; then
    kill_import
    fail "$label: the manifest never reached '$phase' (import finished too fast to interrupt)"
    return 1
  fi
  if [ "$delay" != "0" ]; then
    sleep "$delay"
  fi
  kill_import

  local reached
  reached=$(manifest_phase "$dump")
  note "$label: killed with the manifest at '$reached'"
  assert_eq "$label: the killed run left the manifest at '$phase'" "$reached" "$phase"

  # how much of the data phase had already committed when the kill landed --
  # informational, because it depends on timing, but it is the evidence that
  # says which of the two kill points this run actually hit
  row_counts cs "$tgt" "$WORK/$label.atkill"
  local partial
  partial=$(awk -F'rows=' '{ s += $2 } END { print s + 0 }' "$WORK/$label.atkill")
  note "$label: $partial of $total_rows row(s) were committed when the kill landed"

  # the retry is the same command, typed again -- that is the whole contract
  run_import "$WORK/$label.resume.log" -u dba "$tgt" "$dump"
  assert_rc "$label: the resumed run exits clean" "$IT_RC" 0
  assert_grep "$label: the resumed run says it is resuming" "$WORK/$label.resume.log" \
    "resuming the interrupted import of '$tgt'"
  assert_grep "$label: the resumed run names the phase it restarts from" \
    "$WORK/$label.resume.log" "records phase '$phase' complete"
  assert_grep "$label: the resumed run reports COMPLETE" "$WORK/$label.resume.log" "import COMPLETE"

  catalog_fp cs "$tgt" "$WORK/$label.catalog"
  data_fp cs "$tgt" "$WORK/$label.data"
  assert_same "$label: final catalog equals the uninterrupted baseline" \
    "$WORK/base.catalog" "$WORK/$label.catalog"
  assert_same "$label: final rows equal the uninterrupted baseline" \
    "$WORK/base.data" "$WORK/$label.data"

  # and a third identical run has nothing left to do
  run_import "$WORK/$label.again.log" -u dba "$tgt" "$dump"
  assert_rc "$label: re-running a completed import is a no-op" "$IT_RC" 0
  assert_grep "$label: and says so" "$WORK/$label.again.log" "already fully imported"

  db_drop "$tgt"
  return 0
}

# A -- part-way into the data phase. The manifest reaching "stripped" means the
# strip committed; the extra delay puts the kill inside the load.
kill_at midload it_res_mid stripped 0.2

# B -- at the phase boundary right after the data phase committed.
kill_at boundary it_res_bnd loaded 0

case_exit
