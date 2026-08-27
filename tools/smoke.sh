#!/bin/bash
# End-to-end smoke test: does this build of cubrid-importdb actually import?
#
# Builds a two-table dump (PK, FK, a plain index, 450 rows) with the installed
# unloaddb, imports it into a fresh database with the binary under test, and
# checks the catalog round-trip. This is the acceptance the compile cannot give:
# it exercises discovery, define, graph, plan, strip, load, rebuild, FK validate,
# FK define and stats.
#
# Exit: 0 pass, 1 fail, 77 could not set up (CTest reads 77 as SKIP)
set -uo pipefail
BIN="${1:?usage: smoke.sh <path to cubrid-importdb>}"
[ -x "$BIN" ] || { echo "not executable: $BIN"; exit 77; }
: "${CUBRID:?CUBRID must point at an installed CUBRID}"
export PATH="$CUBRID/bin:$PATH"
export LD_LIBRARY_PATH="$CUBRID/lib:$CUBRID/cci/lib:${LD_LIBRARY_PATH:-}"

W="$(mktemp -d "${TMPDIR:-/tmp}/importdb-smoke.XXXXXX")"
export CUBRID_DATABASES="$W"
cleanup () {
  cubrid server stop smoketgt >/dev/null 2>&1
  cubrid deletedb smoketgt >/dev/null 2>&1
  cubrid deletedb smokesrc >/dev/null 2>&1
  rm -rf "$W"
}
trap cleanup EXIT
mkdir -p "$W/dump"; cd "$W" || exit 77

cubrid createdb --db-volume-size=128M --log-volume-size=64M smokesrc en_US.utf8 >/dev/null 2>&1 || exit 77
csql -u dba -S -c "CREATE TABLE parent (id INTEGER PRIMARY KEY, nm VARCHAR(20));
                   CREATE TABLE child (cid INTEGER PRIMARY KEY, pid INTEGER, v INTEGER,
                     CONSTRAINT fk_c FOREIGN KEY (pid) REFERENCES parent(id));
                   CREATE INDEX i_child_v ON child(v);" smokesrc >/dev/null 2>&1 || exit 77
{
  printf 'INSERT INTO parent VALUES '
  for i in $(seq 1 50); do [ "$i" -gt 1 ] && printf ','; printf "(%d,'p%d')" "$i" "$i"; done
  printf ';\nINSERT INTO child VALUES '
  for i in $(seq 1 400); do [ "$i" -gt 1 ] && printf ','; printf '(%d,%d,%d)' "$i" "$(( (i % 50) + 1 ))" "$i"; done
  printf ';\nCOMMIT;\n'
} > rows.sql
# csql -i exits 0 even when a statement inside the file failed (unlike csql -c,
# which does propagate), so the exit status alone is not a guard here -- check the
# output for an error line as well.
if ! csql -u dba -S --no-auto-commit -i rows.sql smokesrc > rows.out 2>&1 \
     || grep -qE '^ERROR' rows.out; then
  echo "loading the fixture rows failed:"; sed -n '1,10p' rows.out; exit 77
fi
( cd dump && cubrid unloaddb -S -u dba --datafile-per-class smokesrc >/dev/null 2>&1 ) || exit 77
cubrid deletedb smokesrc >/dev/null 2>&1

cubrid createdb --db-volume-size=128M --log-volume-size=64M smoketgt en_US.utf8 >/dev/null 2>&1 || exit 77
cubrid server start smoketgt >/dev/null 2>&1 || exit 77
for _ in $(seq 1 30); do
  csql -u dba -C -t -N -c "SELECT 1 FROM db_root" smoketgt >/dev/null 2>&1 && break
  sleep 1
done

echo "-- importing with $BIN"
"$BIN" -u dba smoketgt "$W/dump" > "$W/import.log" 2>&1
rc=$?
tail -3 "$W/import.log"
[ $rc -eq 0 ] || { echo "FAIL: importdb exited $rc"; sed -n '1,40p' "$W/import.log"; exit 1; }

fail=0
chk () { # name, sql, expected
  got=$(csql -u dba -C -t -N -c "$2" smoketgt 2>/dev/null | tr -d ' \t\n')
  if [ "$got" = "$3" ]; then printf '  PASS  %s (%s)\n' "$1" "$got"
  else printf '  FAIL  %s: got "%s" want "%s"\n' "$1" "$got" "$3"; fail=1; fi
}
chk "parent rows"     "SELECT count(*) FROM parent" 50
chk "child rows"      "SELECT count(*) FROM child"  400
chk "indexes rebuilt" "SELECT count(*) FROM db_index WHERE class_name IN ('parent','child')" 4
chk "pk count"        "SELECT count(*) FROM db_index WHERE class_name IN ('parent','child') AND is_primary_key='YES'" 2
chk "fk defined"      "SELECT count(*) FROM db_index WHERE class_name='child' AND is_foreign_key='YES'" 1
chk "plain index"     "SELECT count(*) FROM db_index WHERE class_name='child' AND index_name='i_child_v'" 1

echo
[ $fail -eq 0 ] && echo "smoke: PASS" || echo "smoke: FAIL"
exit $fail
