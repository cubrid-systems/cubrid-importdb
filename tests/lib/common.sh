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
# databases the SOURCE engine created, when a cross-version case is running
# (see the source-engine section below). Declared here so the cleanup trap's
# reference is in scope no matter where the case stops.
it_src_dbs=()

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
  # A source-engine database has to be dropped by the engine that made it: the
  # target engine would refuse its volume format. The rm -rf below would take
  # the files either way, but deletedb also releases the standalone lock.
  if [ "${#it_src_dbs[@]}" -gt 0 ] && [ -n "${IT_SRC_DBS:-}" ]; then
    for db in "${it_src_dbs[@]}"; do
      src_run it_retry 4 cubrid deletedb "$db" >/dev/null 2>&1
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

# Start a server, and keep what it said. The output used to go to /dev/null,
# which meant a `cannot start <db>` abort named the database and nothing else --
# no way to tell a real refusal from the master having gone down between the
# previous case's cleanup and this call. The log is the difference between
# diagnosing that in one run and guessing at it.
#
# Retried, for the same reason db_create is: on this host the shared cub_master
# comes and goes with the last database on it, and a start that lands in that
# window fails once and succeeds immediately after.
db_start () { # name
  local log="${WORK:-.}/db_start.$1.log" i=0
  : > "$log"
  while [ "$i" -lt 5 ]; do
    if [ "$i" -gt 0 ]; then
      printf -- '--- retry %d ---\n' "$i" >> "$log"
      sleep 2
    fi
    if cubrid server start "$1" >> "$log" 2>&1; then
      db_wait_cs "$1" && return 0
      printf -- '--- started, but no CS connection within the wait ---\n' >> "$log"
    fi
    i=$((i + 1))
  done
  printf '      %-12s db_start %s failed; see %s\n' "${CASE_NAME:-suite}" "$1" "$log"
  sed -n '1,12p' "$log" | sed 's/^/        /'
  return 1
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

# Client-server as a NAMED user rather than DBA, so a case can assert that an
# imported account can actually log in and reach what it was granted. Not
# retried: a wrong password must come back as a refusal, not as five attempts.
sql_cs_as () { # db user password sql
  csql -u "$2" -p "$3" -C -t -N -c "$4" "$1" 2>&1
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

# ------------------------------------------------- the source engine (cross-version)
#
# By default the dump under test is written by the SAME install the target runs
# on ($CUBRID), which is what every same-version case wants. Set IT_SRC_CUBRID
# to a different install and the helpers in this section use that one instead:
# the source database is created, populated and unloaded by the OLD engine, and
# only the import runs against $CUBRID.
#
# The source side never starts a server. createdb, `csql -S` and `unloaddb -S`
# are all standalone, so two installs can sit side by side with nothing to
# coordinate -- no second port, no second cub_master, no lock to fight over.
# That is the whole reason this costs one environment variable instead of a
# second harness.

src_is_foreign () { [ -n "${IT_SRC_CUBRID:-}" ]; }

src_engine_dir () { printf '%s' "${IT_SRC_CUBRID:-$CUBRID}"; }

# A published 10.x binary links libncurses/libform/libtinfo .so.5, which a
# current distribution no longer ships -- it has .so.6. Rather than skip the
# whole lane over a soname, link the .so.6 that IS present under the .so.5 name
# in the scratch directory and put that on the source engine's library path.
# Only the utility front-ends touch these libraries at all, and only for
# terminal handling. If a needed soname has no local counterpart the caller
# still sees the unresolved list and skips with the reason.
src_engine_shims () { # -> prints the shim dir, or nothing
  local shim missing so base dir hit cand
  shim="$IT_SCRATCH/src-shims"
  mkdir -p "$shim" || return 1
  missing=$(LD_LIBRARY_PATH="$(src_engine_dir)/lib:$(src_engine_dir)/cci/lib" \
            ldd "$(src_engine_dir)/bin/cubrid" 2>/dev/null | awk '/not found/ { print $1 }')
  for so in $missing; do
    case "$so" in
      libcubrid*)  continue ;;                 # the engine's own, already on the path
      *.so.[0-9]*) base=${so%.so.*} ;;
      *)           continue ;;
    esac
    hit=""
    for dir in /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /lib64 /usr/lib64; do
      for cand in "$dir/$base".so.[0-9] "$dir/$base".so.[0-9][0-9]; do
        [ -e "$cand" ] && hit=$cand
      done
      [ -n "$hit" ] && break
    done
    [ -n "$hit" ] && ln -sf "$hit" "$shim/$so"
  done
  printf '%s' "$shim"
}

# What is still unresolved for the source engine after the shims. Empty is good.
src_engine_unresolved () {
  local eng
  eng=$(src_engine_dir)
  LD_LIBRARY_PATH="$eng/lib:$eng/cci/lib:${IT_SRC_SHIMS:-}" ldd "$eng/bin/cubrid" 2>/dev/null \
    | awk '/not found/ { printf "%s ", $1 }'
}

# Prepare the source engine: its own databases directory, its own shims. Sets
# IT_SRC_DBS and IT_SRC_SHIMS. Returns non-zero when the engine cannot run here,
# and the caller is expected to skip rather than fail.
src_engine_init () {
  local eng
  eng=$(src_engine_dir)
  if [ ! -x "$eng/bin/cubrid" ]; then
    return 1
  fi
  IT_SRC_DBS="$WORK/src-dbs"
  mkdir -p "$IT_SRC_DBS" || return 1
  IT_SRC_SHIMS=$(src_engine_shims)
  export IT_SRC_DBS IT_SRC_SHIMS
  [ -z "$(src_engine_unresolved)" ] || return 1
  # prove it actually runs, rather than trusting ldd
  src_run cubrid --version >/dev/null 2>&1 || return 1
  return 0
}

# The source engine's full version, out of the line `cubrid --version` prints as
#   CUBRID 10.2 (10.2.18.9024-01b54fa) (64bit release build for Linux) (May ...)
# -- the FIRST parenthesised field, because the later ones are the build date and
# would otherwise win a naive last-field match.
src_engine_version () {
  src_run cubrid --version 2>&1 \
    | sed -n 's/^CUBRID [0-9][0-9.]* (\([^)]*\)).*/\1/p' | head -1
}

# Run one command under the source engine's environment, with cwd set to $1.
# LD_LIBRARY_PATH is replaced, not extended: an old binary that found the
# target install's libraries first would be the one bug this lane cannot afford.
src_run_in () { # dir cmd...
  local dir=$1
  shift
  (
    CUBRID=$(src_engine_dir)
    export CUBRID
    export CUBRID_DATABASES="$IT_SRC_DBS"
    export LD_LIBRARY_PATH="$CUBRID/lib:$CUBRID/cci/lib${IT_SRC_SHIMS:+:$IT_SRC_SHIMS}"
    export PATH="$CUBRID/bin:$PATH"
    cd "$dir" || exit 1
    "$@"
  )
}

src_run () { src_run_in "$IT_SRC_DBS" "$@"; }

src_db_create () { # name [volume-size] [log-size]
  local name=$1 vol=${2:-64M} log=${3:-32M}
  it_src_dbs+=("$name")
  src_run it_retry 10 cubrid createdb --db-volume-size="$vol" --log-volume-size="$log" \
      "$name" en_US.utf8 >/dev/null 2>&1
}

src_sql () { # db sql
  src_run csql -u dba -S -t -N -c "$2" "$1" 2>&1
}

src_sql_script () { # db file
  src_run csql -u dba -S -t -N -i "$2" "$1" 2>&1
}

src_sql_load () { # db file
  src_run csql -u dba -S --no-auto-commit -i "$2" "$1" 2>&1
}

src_fixture_apply () { # db fixture
  src_sql_script "$1" "$IT_FIXTURES_DIR/$2.sql" > "$WORK/src.$2.ddl.log" 2>&1
  ! grep -qi 'ERROR' "$WORK/src.$2.ddl.log"
}

src_fixture_rows () { # db fixture [rows]
  local db=$1 fx=$2
  shift 2
  bash "$IT_FIXTURES_DIR/gen_rows.sh" "$fx" "$@" > "$WORK/src.$fx.rows.sql" || return 1
  src_sql_load "$db" "$WORK/src.$fx.rows.sql" > "$WORK/src.$fx.rows.log" 2>&1
  ! grep -qi 'ERROR' "$WORK/src.$fx.rows.log"
}

# Roster a dump with the SOURCE engine's unloaddb, into a directory the target
# engine will later read.
src_unload_dump () { # db dir extra-args...
  local db=$1 dir=$2
  shift 2
  mkdir -p "$dir" || return 1
  src_run_in "$dir" cubrid unloaddb -S -u dba "$@" "$db" >/dev/null 2>&1
}

# The data half of the fingerprint, read through the source engine. Byte-for-byte
# comparable with data_fp's output: same query, same csql flags, same filter.
src_data_fp () { # db outfile
  local db=$1 out=$2 cls
  : > "$out"
  src_sql "$db" "SELECT class_name FROM db_class WHERE is_system_class='NO' AND class_type='CLASS' ORDER BY class_name" \
    | awk 'NF > 0 { print $1 }' > "$out.classes"
  while read -r cls; do
    [ -n "$cls" ] || continue
    src_sql "$db" "SELECT * FROM $cls" | awk 'NF > 0' > "$out.rows"
    printf '%s rows=%s sha=%s\n' "$cls" "$(wc -l < "$out.rows" | tr -d ' ')" \
      "$(LC_ALL=C sort "$out.rows" | sha256sum | cut -c1-32)" >> "$out"
  done < "$out.classes"
}

# Users and their grants on the dump's own classes, on a TARGET database. Not in
# fingerprint.sql because the system users' grants on catalog classes are noise
# there; here the point is exactly that a user and its privileges came back.
auth_fp () { # db outfile
  {
    sql_cs "$1" "SELECT '#USER', name FROM db_user ORDER BY 1, 2"
    sql_cs "$1" "SELECT '#GRANT', a.grantee_name, a.object_name, a.auth_type
                   FROM db_auth a, db_class c
                  WHERE a.object_name = c.class_name AND c.is_system_class = 'NO'
                  ORDER BY 2, 3, 4"
  } | awk 'NF > 0' > "$2"
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
