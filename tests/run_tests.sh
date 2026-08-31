#!/bin/bash
# run_tests.sh -- the cubrid-importdb functional suite.
#
#   tests/run_tests.sh                       run every case
#   tests/run_tests.sh roundtrip fkcycle     run a subset
#   tests/run_tests.sh -k resume             keep the scratch dir for debugging
#   tests/run_tests.sh -l                    list the cases
#   tests/run_tests.sh --bin=/path/to/cubrid-importdb
#
# Each case is a separate process under tests/cases/. It prints one
# PASS/FAIL/SKIP line per assertion; this script tallies them, prints the
# totals, and exits non-zero if anything failed. A case that dies without
# printing a verdict counts as one failure.
#
# Exit: 0 all passed, 1 something failed, 77 the environment cannot run the
# suite at all (CTest reads 77 as SKIP).
set -uo pipefail

IT_SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
. "$IT_SELF_DIR/lib/common.sh"

IT_ALL_CASES="roundtrip types ordering fkcycle fkviolation dryrun degree resume refusals compat perf"

usage () {
  sed -n '2,16p' "$IT_SELF_DIR/run_tests.sh" | sed 's/^# \{0,1\}//'
}

IT_KEEP=0
selected=()
while [ $# -gt 0 ]; do
  case "$1" in
    -k|--keep)   IT_KEEP=1 ;;
    -l|--list)   printf '%s\n' $IT_ALL_CASES; exit 0 ;;
    -h|--help)   usage; exit 0 ;;
    --bin)       IMPORTDB_BIN=$2; shift ;;
    --bin=*)     IMPORTDB_BIN=${1#--bin=} ;;
    -*)          echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)           selected+=("$1") ;;
  esac
  shift
done
export IT_KEEP
# Absolutize before exporting: every case cd's into its own scratch directory, so
# a relative --bin (the form this script's own usage documents) would resolve
# against the wrong directory and fail with 127.
if [ -n "${IMPORTDB_BIN:-}" ]; then
  case "$IMPORTDB_BIN" in
    /*) ;;
    */*) IMPORTDB_BIN=$(cd "$(dirname "$IMPORTDB_BIN")" && pwd)/$(basename "$IMPORTDB_BIN") ;;
  esac
  export IMPORTDB_BIN
fi

if [ "${#selected[@]}" -eq 0 ]; then
  read -r -a cases <<< "$IT_ALL_CASES"
else
  cases=("${selected[@]}")
fi
for c in "${cases[@]}"; do
  if [ ! -f "$IT_SELF_DIR/cases/$c.sh" ]; then
    echo "no such case: $c (try --list)" >&2
    exit 2
  fi
done

# --------------------------------------------------------------- can we run?

# The scratch root is created here, before it_setup_env, so that helper does
# not make a second one of its own -- and it is trapped immediately, so even a
# refusal below does not leave it behind.
IT_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/importdb-tests.XXXXXX")" || exit 77
export IT_SCRATCH
suite_cleanup () {
  if [ "$IT_KEEP" = "1" ]; then
    echo "scratch kept: $IT_SCRATCH"
  else
    rm -rf "$IT_SCRATCH"
  fi
}
trap suite_cleanup EXIT

# it_setup_env also refuses a scratch root inside the repo: a CUBRID volume is
# 64 MB or more and GitHub will not take it.
if ! it_setup_env; then
  echo "functional suite skipped: environment not usable" >&2
  exit 77
fi
if ! command -v cubrid >/dev/null 2>&1; then
  echo "functional suite skipped: no cubrid in PATH" >&2
  exit 77
fi

# Preflight: prove a database can actually be created and served here before
# blaming a case for the environment.
preflight () {
  local pf="$IT_SCRATCH/preflight" ok=1
  mkdir -p "$pf" || return 1
  (
    cd "$pf" || exit 1
    export CUBRID_DATABASES="$pf"
    cubrid createdb --db-volume-size=64M --log-volume-size=32M it_preflight en_US.utf8 >/dev/null 2>&1 || exit 1
    cubrid server start it_preflight >/dev/null 2>&1 || { cubrid deletedb it_preflight >/dev/null 2>&1; exit 1; }
    csql -u dba -C -t -N -c "SELECT 1 FROM db_root" it_preflight >/dev/null 2>&1
    rc=$?
    cubrid server stop it_preflight >/dev/null 2>&1
    it_retry 8 cubrid deletedb it_preflight >/dev/null 2>&1
    exit $rc
  ) || ok=0
  rm -rf "$pf"
  [ "$ok" = "1" ]
}

echo "cubrid-importdb functional suite"
echo "  binary : $IMPORTDB_BIN"
echo "  CUBRID : $CUBRID"
echo "  scratch: $IT_SCRATCH"
echo "  cases  : ${cases[*]}"
echo

if ! preflight; then
  echo "functional suite skipped: cannot create and serve a database here" >&2
  exit 77
fi

# ------------------------------------------------------------------- the cases

log="$IT_SCRATCH/suite.log"
: > "$log"
aborted=0
case_failed=0

for c in "${cases[@]}"; do
  printf '== %s ==\n' "$c"
  started=$SECONDS
  bash "$IT_SELF_DIR/cases/$c.sh" 2>&1 | tee -a "$log"
  rc=${PIPESTATUS[0]}
  elapsed=$((SECONDS - started))
  if [ "$rc" -ne 0 ]; then
    case_failed=$((case_failed + 1))
    if ! awk -v c="$c" '$1 == "FAIL" && $2 == c { f = 1 } END { exit !f }' "$log"; then
      printf 'FAIL  %-12s case exited %d without reporting a failure\n' "$c" "$rc" | tee -a "$log"
      aborted=$((aborted + 1))
    fi
  fi
  printf '   (%s: %ds)\n\n' "$c" "$elapsed"
done

passed=$(awk '$1 == "PASS"' "$log" | wc -l | tr -d ' ')
failed=$(awk '$1 == "FAIL"' "$log" | wc -l | tr -d ' ')
skipped=$(awk '$1 == "SKIP"' "$log" | wc -l | tr -d ' ')

echo "---- tally ----"
printf 'assertions : %s passed, %s failed, %s skipped\n' "$passed" "$failed" "$skipped"
printf 'cases      : %d run, %d with failures, %d aborted\n' \
  "${#cases[@]}" "$case_failed" "$aborted"
if [ "$failed" -eq 0 ] && [ "$case_failed" -eq 0 ]; then
  echo "functional: PASS"
  exit 0
fi
echo "functional: FAIL"
awk '$1 == "FAIL"' "$log" | sed 's/^/  /'
exit 1
