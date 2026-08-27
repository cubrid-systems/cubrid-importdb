#!/bin/bash
# common.sh -- shared helpers for the cubrid-importdb functional suite.
#
# Sourced by tests/run_tests.sh and by every tests/cases/*.sh, so a single case
# can also be run on its own for debugging:
#
#     CUBRID=/path/to/install IMPORTDB_BIN=/path/to/cubrid-importdb \
#         bash tests/cases/roundtrip.sh
#
# The assertion vocabulary is deliberately small -- assert_eq, assert_rc,
# assert_grep, assert_no_grep, assert_same, assert_file, assert_no_file -- plus
# pass/fail/skip/note for the few things that need to speak for themselves.
# Every assertion prints exactly one PASS/FAIL line; run_tests.sh tallies those.
#
# SAFETY RULES this file enforces, because breaking either of them has already
# cost the project a day:
#   * every database lives in a scratch directory OUTSIDE the repo, created per
#     run and removed on exit. A 128 MB CUBRID volume committed by accident is
#     rejected by GitHub's 100 MB file limit.
#   * only databases this suite created are ever stopped or deleted. db_create
#     records the name; the cleanup trap walks that list and nothing else.

# shellcheck shell=bash

IT_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IT_TESTS_DIR="$(dirname "$IT_LIB_DIR")"
IT_REPO_DIR="$(dirname "$IT_TESTS_DIR")"
export IT_LIB_DIR IT_TESTS_DIR IT_REPO_DIR

IT_FIXTURES_DIR="$IT_TESTS_DIR/fixtures"
export IT_FIXTURES_DIR

# assertion tallies for the current process (one case, or the runner itself)
it_pass_count=0
it_fail_count=0

# databases this process created (see it_case_cleanup)
it_dbs=()

# set by run_import / run_import_bg, read by the cases
IT_RC=0
IT_PID=0
export IT_RC IT_PID

# ------------------------------------------------------------------ reporting

pass () {
  it_pass_count=$((it_pass_count + 1))
  printf 'PASS  %-12s %s\n' "${CASE_NAME:-suite}" "$1"
}

fail () {
  it_fail_count=$((it_fail_count + 1))
  printf 'FAIL  %-12s %s\n' "${CASE_NAME:-suite}" "$1"
}

skip () {
  printf 'SKIP  %-12s %s\n' "${CASE_NAME:-suite}" "$1"
}

note () {
  printf '      %-12s %s\n' "${CASE_NAME:-suite}" "$1"
}

die () {
  printf 'ABORT %-12s %s\n' "${CASE_NAME:-suite}" "$1"
  exit 1
}

# ----------------------------------------------------------------- assertions

assert_eq () { # name got want
  if [ "$2" = "$3" ]; then
    pass "$1 ($2)"
  else
    fail "$1: got '$2' want '$3'"
  fi
}

assert_rc () { # name got want
  if [ "$2" = "$3" ]; then
    pass "$1 (exit $2)"
  else
    fail "$1: exited $2, expected $3"
  fi
}

assert_nonzero_rc () { # name got
  if [ "$2" != "0" ]; then
    pass "$1 (exit $2)"
  else
    fail "$1: exited 0, expected non-zero"
  fi
}

assert_grep () { # name file regex
  if [ -f "$2" ] && grep -Eq -- "$3" "$2"; then
    pass "$1"
  else
    fail "$1: '$3' not found in $2"
  fi
}

assert_no_grep () { # name file regex
  if [ ! -f "$2" ] || ! grep -Eq -- "$3" "$2"; then
    pass "$1"
  else
    fail "$1: '$3' unexpectedly present in $2"
  fi
}

assert_same () { # name file_expected file_actual
  if diff -q "$2" "$3" >/dev/null 2>&1; then
    pass "$1"
  else
    fail "$1: $2 and $3 differ"
    diff -u "$2" "$3" 2>&1 | sed -n '1,14p' | sed 's/^/        /'
  fi
}

assert_file () { # name path
  if [ -f "$2" ]; then
    pass "$1"
  else
    fail "$1: $2 does not exist"
  fi
}

assert_no_file () { # name path
  if [ ! -e "$2" ]; then
    pass "$1"
  else
    fail "$1: $2 exists and should not"
  fi
}

# --------------------------------------------------------------- environment

# Resolve $CUBRID, the runtime library path and the binary under test. Returns
# 1 when the environment cannot support the suite at all; the caller turns that
# into exit 77 (CTest reads 77 as SKIP).
it_setup_env () {
  if [ -z "${CUBRID:-}" ]; then
    echo "cannot run: CUBRID must point at an installed CUBRID" >&2
    return 1
  fi
  if [ ! -x "$CUBRID/bin/csql" ]; then
    echo "cannot run: no csql under $CUBRID/bin" >&2
    return 1
  fi
  PATH="$CUBRID/bin:$PATH"
  LD_LIBRARY_PATH="$CUBRID/lib:$CUBRID/cci/lib:${LD_LIBRARY_PATH:-}"
  export PATH LD_LIBRARY_PATH

  if [ -z "${IMPORTDB_BIN:-}" ]; then
    for c in "$IT_REPO_DIR/build/cubrid-importdb" "$(command -v cubrid-importdb || true)"; do
      if [ -n "$c" ] && [ -x "$c" ]; then
        IMPORTDB_BIN="$c"
        break
      fi
    done
  fi
  if [ -z "${IMPORTDB_BIN:-}" ] || [ ! -x "$IMPORTDB_BIN" ]; then
    echo "cannot run: no cubrid-importdb binary (set IMPORTDB_BIN or --bin)" >&2
    return 1
  fi
  export IMPORTDB_BIN

  # The scratch root MUST be outside the repo: a CUBRID volume is ~128 MB and
  # GitHub refuses files over 100 MB, so a database created in the worktree
  # breaks the push. run_tests.sh normally sets this; honour it if present.
  if [ -z "${IT_SCRATCH:-}" ]; then
    IT_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/importdb-tests.XXXXXX")" || return 1
    export IT_SCRATCH
    IT_SCRATCH_OWNED=1
    export IT_SCRATCH_OWNED
  fi
  case "$IT_SCRATCH" in
    "$IT_REPO_DIR"/*)
      echo "cannot run: scratch dir $IT_SCRATCH is inside the repo" >&2
      return 1
      ;;
  esac
  return 0
}

# ------------------------------------------------------------- case lifecycle

# Give the case its own scratch directory and its own CUBRID_DATABASES, and
# arm the cleanup trap. Every cubrid command below runs with WORK as the cwd,
# so nothing a database or a dump writes can land in the repo.
case_init () {
  it_setup_env || die "environment not usable"
  WORK="$IT_SCRATCH/${CASE_NAME:?CASE_NAME must be set before case_init}"
  rm -rf "$WORK"
  mkdir -p "$WORK" || die "cannot create $WORK"
  CUBRID_DATABASES="$WORK"
  export WORK CUBRID_DATABASES
  cd "$WORK" || die "cannot cd to $WORK"
  trap it_case_cleanup EXIT
}

# Stop and delete only the databases db_create recorded, then drop the case's
# scratch directory unless the caller asked to keep it.
it_case_cleanup () {
  local db
  if [ "${#it_dbs[@]}" -gt 0 ]; then
    for db in "${it_dbs[@]}"; do
      cubrid server stop "$db" >/dev/null 2>&1
      it_retry 8 cubrid deletedb "$db" >/dev/null 2>&1
    done
  fi
  cd / || true
  if [ "${IT_KEEP:-0}" = "1" ]; then
    printf '      %-12s scratch kept: %s\n' "${CASE_NAME:-suite}" "$WORK"
  else
    rm -rf "$WORK"
    # A case run on its own (not through run_tests.sh) made the scratch root
    # itself, so it has to take it away again.
    if [ "${IT_SCRATCH_OWNED:-0}" = "1" ]; then
      rmdir "$IT_SCRATCH" 2>/dev/null
    fi
  fi
}

# Exit with the case's verdict: 0 when nothing failed, 1 otherwise.
case_exit () {
  if [ "$it_fail_count" -eq 0 ]; then
    exit 0
  fi
  exit 1
}

# ------------------------------------------------------------------- database

# Run a command up to N times, 1s apart, until it succeeds. The host holds a
# database briefly after `cubrid server stop`, so deletedb/createdb right
# after a stop legitimately fail once or twice.
it_retry () { # tries cmd...
  local tries=$1 i=0
  shift
  while [ "$i" -lt "$tries" ]; do
    if "$@"; then
      return 0
    fi
    i=$((i + 1))
    sleep 1
  done
  return 1
}

db_create () { # name [volume-size] [log-size]
  local name=$1 vol=${2:-64M} log=${3:-32M}
  it_dbs+=("$name")
  if ! it_retry 10 cubrid createdb --db-volume-size="$vol" --log-volume-size="$log" \
      "$name" en_US.utf8 >/dev/null 2>&1; then
    return 1
  fi
  return 0
}

db_start () { # name
  cubrid server start "$1" >/dev/null 2>&1 || return 1
  db_wait_cs "$1"
}

db_stop () { # name
  cubrid server stop "$1" >/dev/null 2>&1
  return 0
}

# A fresh CS connection right after a server start (or right after a bulk load)
# intermittently fails on this host, so every client-server entry point waits.
db_wait_cs () { # name
  local i=0
  while [ "$i" -lt 60 ]; do
    if csql -u dba -C -t -N -c "SELECT 1 FROM db_root" "$1" >/dev/null 2>&1; then
      return 0
    fi
    i=$((i + 1))
    sleep 1
  done
  return 1
}

# Drop a database this suite created, and forget it, so the cleanup trap does
# not spend its retry budget deleting something that is already gone.
db_drop () { # name
  local db keep=()
  cubrid server stop "$1" >/dev/null 2>&1
  it_retry 8 cubrid deletedb "$1" >/dev/null 2>&1
  if [ "${#it_dbs[@]}" -gt 0 ]; then
    for db in "${it_dbs[@]}"; do
      [ "$db" = "$1" ] || keep+=("$db")
    done
  fi
  it_dbs=("${keep[@]+${keep[@]}}")
  return 0
}

# ------------------------------------------------------------------------ SQL

# Standalone (server down). Autocommit is on, which is what the fixture DDL
# wants.
sql_sa () { # db sql
  csql -u dba -S -t -N -c "$2" "$1" 2>&1
}

# -t -N so the output matches sql_cs_script's: the fingerprint comparison is
# byte-for-byte across the two modes.
sql_sa_script () { # db file
  csql -u dba -S -t -N -i "$2" "$1" 2>&1
}

# Bulk row inserts: one transaction, the file ends with COMMIT.
sql_sa_load () { # db file
  csql -u dba -S --no-auto-commit -i "$2" "$1" 2>&1
}

# Client-server (server up), retried -- see db_wait_cs.
sql_cs () { # db sql
  local i=0 out
  while [ "$i" -lt 5 ]; do
    if out=$(csql -u dba -C -t -N -c "$2" "$1" 2>&1); then
      printf '%s\n' "$out"
      return 0
    fi
    i=$((i + 1))
    sleep 1
  done
  printf '%s\n' "$out"
  return 1
}

sql_cs_script () { # db file
  local i=0 out
  while [ "$i" -lt 5 ]; do
    if out=$(csql -u dba -C -t -N -i "$2" "$1" 2>&1); then
      printf '%s\n' "$out"
      return 0
    fi
    i=$((i + 1))
    sleep 1
  done
  printf '%s\n' "$out"
  return 1
}

# One scalar, whitespace stripped -- the shape almost every assertion wants.
q1 () { # mode db sql
  local out
  if [ "$1" = "sa" ]; then
    out=$(sql_sa "$2" "$3")
  else
    out=$(sql_cs "$2" "$3")
  fi
  printf '%s' "$out" | tr -d ' \t\n'
}

# ---------------------------------------------------------------- fingerprints

# Every user class, in a stable order. Partition sub-classes (t__p__p0) are
# real classes and are included: both sides of a comparison see them.
class_list () { # mode db
  local sql="SELECT class_name FROM db_class WHERE is_system_class='NO' AND class_type='CLASS' ORDER BY class_name"
  if [ "$1" = "sa" ]; then
    sql_sa "$2" "$sql"
  else
    sql_cs "$2" "$sql"
  fi | awk 'NF > 0 { print $1 }'
}

# The catalog half of the round-trip claim: classes, attributes with their
# domains, the full index set (PK/UK/FK/plain/reverse with their key columns),
# inheritance, partitions, serials and triggers. Timestamps are excluded --
# they legitimately differ between the source and its import.
catalog_fp () { # mode db outfile
  if [ "$1" = "sa" ]; then
    sql_sa_script "$2" "$IT_LIB_DIR/fingerprint.sql"
  else
    sql_cs_script "$2" "$IT_LIB_DIR/fingerprint.sql"
  fi | awk 'NF > 0' > "$3"
}

# The data half: per class, the row count and an order-independent content
# checksum. The physical load order differs from the source's, so the rows are
# sorted before hashing.
#
# One query per class: the count is the number of rows fetched, which holds
# because no fixture puts a newline inside a value. row_counts () asks the
# server for the counts instead, and the roundtrip case compares the two.
data_fp () { # mode db outfile
  local mode=$1 db=$2 out=$3 cls
  : > "$out"
  class_list "$mode" "$db" > "$out.classes"
  while read -r cls; do
    [ -n "$cls" ] || continue
    if [ "$mode" = "sa" ]; then
      sql_sa "$db" "SELECT * FROM $cls"
    else
      sql_cs "$db" "SELECT * FROM $cls"
    fi | awk 'NF > 0' > "$out.rows"
    printf '%s rows=%s sha=%s\n' "$cls" "$(wc -l < "$out.rows" | tr -d ' ')" \
      "$(LC_ALL=C sort "$out.rows" | sha256sum | cut -c1-32)" >> "$out"
  done < "$out.classes"
}

# Per-class row counts asked of the server, independent of the rows data_fp
# hashed. Used where the count itself is the claim.
row_counts () { # mode db outfile
  local mode=$1 db=$2 out=$3 cls
  : > "$out"
  class_list "$mode" "$db" > "$out.classes"
  while read -r cls; do
    [ -n "$cls" ] || continue
    printf '%s rows=%s\n' "$cls" "$(q1 "$mode" "$db" "SELECT count(*) FROM $cls")" >> "$out"
  done < "$out.classes"
}

# ------------------------------------------------------------------- the tool

run_import () { # logfile args...
  local log=$1
  shift
  "$IMPORTDB_BIN" "$@" > "$log" 2>&1
  IT_RC=$?
  return 0
}

run_import_bg () { # logfile args...
  local log=$1
  shift
  "$IMPORTDB_BIN" "$@" > "$log" 2>&1 &
  IT_PID=$!
}

# Kill the backgrounded import by PID. Never by pattern: a pattern wide enough
# to match the utility also matches this harness.
kill_import () {
  kill -9 "$IT_PID" 2>/dev/null
  wait "$IT_PID" 2>/dev/null
  return 0
}

# Wait until the manifest records a phase complete. The manifest is written
# temp+rename, so a read never sees a partial file, and "current: <phase>"
# means that phase's work is already committed.
wait_phase () { # dumpdir phase tries
  local m="$1/importdb.manifest" want=$2 tries=$3 i=0
  while [ "$i" -lt "$tries" ]; do
    if [ -f "$m" ] && awk -v w="$want" '$0 == "current: " w { f = 1 } END { exit !f }' "$m" 2>/dev/null; then
      return 0
    fi
    i=$((i + 1))
    sleep 0.02
  done
  return 1
}

manifest_phase () { # dumpdir
  awk '/^current: / { print $2; exit }' "$1/importdb.manifest" 2>/dev/null
}

# Roster a dump with the installed unloaddb. Runs inside the dump directory
# because that is where unloaddb writes.
unload_dump () { # db dir extra-args...
  local db=$1 dir=$2
  shift 2
  mkdir -p "$dir" || return 1
  ( cd "$dir" && cubrid unloaddb -S -u dba "$@" "$db" >/dev/null 2>&1 )
}

# The dump prefix unloaddb uses is the source database name.
fixture_apply () { # db fixture
  sql_sa_script "$1" "$IT_FIXTURES_DIR/$2.sql" > "$WORK/$2.ddl.log" 2>&1
  if grep -qi 'ERROR' "$WORK/$2.ddl.log"; then
    return 1
  fi
  return 0
}

fixture_rows () { # db fixture [rows]
  local db=$1 fx=$2
  shift 2
  bash "$IT_FIXTURES_DIR/gen_rows.sh" "$fx" "$@" > "$WORK/$fx.rows.sql" || return 1
  sql_sa_load "$db" "$WORK/$fx.rows.sql" > "$WORK/$fx.rows.log" 2>&1
  if grep -qi 'ERROR' "$WORK/$fx.rows.log"; then
    return 1
  fi
  return 0
}
