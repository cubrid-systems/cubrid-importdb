#!/bin/bash
# Run the upstream contract check against a scratch database.
#
# Everything it needs is an installed CUBRID ($CUBRID). It creates its own
# database and its own dump, so it can run on a bare CI runner. Exit codes:
#   0  every check passed
#   1  a check failed  -> the contract with upstream is broken, read the output
#  77  could not set up (no $CUBRID, server refused to start) -> CTest SKIP
set -uo pipefail
: "${CUBRID:?CUBRID is not set -- point it at an installed CUBRID}"
export PATH="$CUBRID/bin:$PATH"
export LD_LIBRARY_PATH="$CUBRID/lib:$CUBRID/cci/lib:${LD_LIBRARY_PATH:-}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${CONTRACT_CHECK_BIN:-$HERE/../build/contract_check}"
[ -x "$BIN" ] || { echo "contract_check not built at $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cubrid-contract.XXXXXX")"
DB=ccheck
export CUBRID_DATABASES="$WORK"
cleanup () {
  cubrid server stop "$DB"    >/dev/null 2>&1
  cubrid server stop ccsrc    >/dev/null 2>&1
  cubrid deletedb  "$DB"      >/dev/null 2>&1
  cubrid deletedb  ccsrc      >/dev/null 2>&1
  rm -rf "$WORK"
}
trap cleanup EXIT
cd "$WORK" || exit 77

echo "-- building a scratch dump (setup, not part of the contract)"
cubrid createdb --db-volume-size=64M --log-volume-size=32M ccsrc en_US.utf8 >/dev/null 2>&1 || exit 77
csql -u dba -S -c "CREATE TABLE t_narrow (a INTEGER, b INTEGER, c INTEGER, d INTEGER, e INTEGER, f INTEGER, g INTEGER, h INTEGER);" ccsrc >/dev/null 2>&1 || exit 77
{
  printf 'INSERT INTO t_narrow VALUES '
  for i in $(seq 1 500); do
    [ "$i" -gt 1 ] && printf ','
    printf '(%d,%d,%d,%d,%d,%d,%d,%d)' "$i" "$i" "$i" "$i" "$i" "$i" "$i" "$i"
  done
  printf ';\nCOMMIT;\n'
} > rows.sql
csql -u dba -S --no-auto-commit -i rows.sql ccsrc >/dev/null 2>&1 || exit 77
mkdir -p dump && ( cd dump && cubrid unloaddb -S -u dba --datafile-per-class ccsrc >/dev/null 2>&1 ) || exit 77
OBJ="$(ls "$WORK"/dump/*_objects 2>/dev/null | head -1)"
cubrid deletedb ccsrc >/dev/null 2>&1
[ -n "$OBJ" ] || exit 77

cubrid createdb --db-volume-size=64M --log-volume-size=32M "$DB" en_US.utf8 >/dev/null 2>&1 || exit 77
# check 04 asserts that the db_serial VIEW is narrower than the _db_serial CLASS
# (the view omits AUTO_INCREMENT serials, which is why the Graph builder has to
# read the class). Give the target one of each so the check is not vacuous.
csql -u dba -S -c "CREATE SERIAL cc_seq; CREATE TABLE cc_ai (x INTEGER AUTO_INCREMENT PRIMARY KEY);" "$DB" >/dev/null 2>&1 || exit 77
cubrid server start "$DB" >/dev/null 2>&1 || exit 77
# wait for the server to accept a CS connection
for _ in $(seq 1 30); do
  csql -u dba -C -t -N -c "SELECT 1 FROM db_root" "$DB" >/dev/null 2>&1 && break
  sleep 1
done

# CONTRACT_NEGATIVE=1 runs the same checks as a user who is NOT in the DBA
# group. It must FAIL -- that is what proves the suite still discriminates
# rather than passing unconditionally. CI inverts the exit code for this mode.
if [ "${CONTRACT_NEGATIVE:-0}" = 1 ]; then
  csql -u dba -C -c "CREATE USER cc_nodba PASSWORD 'p';" "$DB" >/dev/null 2>&1
  echo "-- negative control: running the contract as cc_nodba (not in DBA group)"
  echo
  CONTRACT_USER=cc_nodba CONTRACT_PASSWORD=p "$BIN" "$DB" "$OBJ"
  rc=$?
  echo
  echo "-- contract_check exit=$rc (a non-zero exit here is the expected result)"
  exit $rc
fi

echo
"$BIN" "$DB" "$OBJ"
rc=$?
echo
echo "-- contract_check exit=$rc"
exit $rc
