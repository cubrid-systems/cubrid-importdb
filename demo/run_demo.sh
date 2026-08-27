#!/bin/bash
# demo/run_demo.sh -- cubrid-importdb, shown rather than described.
#
# Starts from what `cubrid unloaddb -S -u dba <db>` actually leaves behind: THREE
# files -- <db>_schema, <db>_objects, <db>_indexes. One object file for the whole
# database. That is the dump an operator has in hand, so it is the dump the demo
# reloads. `--datafile-per-class` is a non-default option, and it shows up only
# in scenario 4, where it is the subject rather than the fixture.
#
#   1  baseline contrast   hand-scripted `cubrid loaddb -s / -d / -i` vs one
#                          `cubrid-importdb` invocation -- same rows, same
#                          constraint set, same serial
#   2  the plan            --dry-run prints the dependency levels and the
#                          terminal task order, and changes nothing
#   3  the FK violation    a dump carrying orphan rows: loaddb accepts it and
#                          leaves a FOREIGN KEY the engine itself would refuse
#                          to create; importdb enumerates the offenders and
#                          withholds that FK
#   4  parallelism         --degree=4 twice: on the default dump, where there is
#                          one object file and the degree quietly clamps to
#                          serial, and on a --datafile-per-class dump, where the
#                          fan-out actually happens. The dump layout is what
#                          buys parallelism, not the flag.
#
# Every database and directory it touches is its own; nothing pre-existing is
# read or written. Cleans up on exit.
#
# usage:  CUBRID=/path/to/install bash demo/run_demo.sh [-k] [path to cubrid-importdb]
#           -k, --keep, or KEEP=1   leave the scratch directory behind for
#                                   inspection instead of deleting it
# exit:   0 all assertions passed, 1 an assertion failed, 77 could not set up
#         (CTest reads 77 as SKIP)

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

KEEP="${KEEP:-0}"
while [ "$#" -gt 0 ]; do
  case "$1" in
    -k|--keep) KEEP=1; shift ;;
    -*) echo "demo: unknown option '$1'"; exit 77 ;;
    *) break ;;
  esac
done

BIN="${1:-${IMPORTDB_BIN:-}}"
if [ -z "$BIN" ] && [ -x "$HERE/../build/cubrid-importdb" ]; then
  BIN="$HERE/../build/cubrid-importdb"
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "demo: no cubrid-importdb binary."
  echo "      usage: CUBRID=<install> bash demo/run_demo.sh [-k] <path to cubrid-importdb>"
  echo "      (or set IMPORTDB_BIN, or build into ./build/cubrid-importdb)"
  exit 77
fi
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
: "${CUBRID:?CUBRID must point at an installed CUBRID}"
export PATH="$CUBRID/bin:$PATH"
export LD_LIBRARY_PATH="$CUBRID/lib:$CUBRID/cci/lib:${LD_LIBRARY_PATH:-}"
command -v csql >/dev/null 2>&1 || { echo "demo: csql not on PATH under \$CUBRID/bin"; exit 77; }
[ -r "$HERE/schema.sql" ] || { echo "demo: demo/schema.sql missing"; exit 77; }
[ -r "$HERE/gen_rows.sh" ] || { echo "demo: demo/gen_rows.sh missing"; exit 77; }

# The databases this demo creates. Names are demo-private and they live inside
# a scratch $CUBRID_DATABASES this script makes and removes, so a database that
# already exists elsewhere is never reachable, let alone touched.
SRC=idbdemo_src        # the database that gets unloaded
OLD=idbdemo_old        # scenario 1, reloaded the hand-scripted way
NEW=idbdemo_new        # scenario 1, reloaded with cubrid-importdb
DRY=idbdemo_dry        # scenario 2, --dry-run target
FKOLD=idbdemo_fkold    # scenario 3, orphan dump the hand-scripted way
FKNEW=idbdemo_fknew    # scenario 3, orphan dump with cubrid-importdb
PARD=idbdemo_pardef    # scenario 4a, --degree=4 on the default dump
PARP=idbdemo_parpc     # scenario 4b, --degree=4 on a per-class dump

W="$(mktemp -d "${TMPDIR:-/tmp}/importdb-demo.XXXXXX")" || exit 77
LOGS="$W/logs"
# The two dumps stage 0 produces. Both are read-only fixtures: every scenario
# runs against its own copy, because importdb writes importdb.manifest (and, on
# a violation, importdb.exceptions) next to the dump, and a leftover manifest is
# exactly what turns the next run into a resume.
DUMP="$W/dump-default"          # plain `unloaddb -S -u dba <db>`: three files
DUMP_PC="$W/dump-per-class"     # ... --datafile-per-class: one object file per class
DUMP_S1="$W/s1-import"          # scenario 1  (copy of DUMP)
DUMP_S2="$W/s2-dryrun"          # scenario 2  (copy of DUMP)
DUMP_S3="$W/s3-orphan"          # scenario 3  (copy of DUMP, orphan rows injected)
DUMP_S4A="$W/s4a-default"       # scenario 4a (copy of DUMP)
DUMP_S4B="$W/s4b-per-class"     # scenario 4b (copy of DUMP_PC)
mkdir -p "$LOGS" "$DUMP" "$DUMP_PC" || exit 77

# BOTH of these matter. $CUBRID_DATABASES is where CUBRID keeps databases.txt
# and looks for a database by name; the *volumes* are created relative to the
# working directory, and csql/loaddb also drop csql.err, csql.access, lob/ and
# their own .log files there. Leaving either pointed at a source tree litters it
# with 128 MB volume files.
export CUBRID_DATABASES="$W"
cd "$W" || exit 77

# Only databases this run actually created are ever stopped or dropped. A name
# is recorded before createdb runs, so a half-created one is still cleaned up.
CREATED_DBS=""

cleanup () {
  local db
  for db in $CREATED_DBS; do
    cubrid server stop "$db" >/dev/null 2>&1
    if [ "$KEEP" != "1" ]; then
      cubrid deletedb "$db" >/dev/null 2>&1
    fi
  done
  cd / || return 0
  if [ "$KEEP" = "1" ]; then
    echo
    echo "demo: KEEP -- scratch directory left behind: $W"
    echo "      databases:  ${CREATED_DBS# }  (servers stopped; CUBRID_DATABASES=$W)"
    echo "      logs:       $W/logs"
    echo "      dumps:      $W/dump-default $W/dump-per-class (+ one copy per scenario)"
    echo "      remove it with: rm -rf $W"
  else
    rm -rf "$W"
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- output ----

banner () {
  echo
  echo "=============================================================================="
  echo "  $*"
  echo "=============================================================================="
}

step () { echo; echo "-- $*"; }

PASS=0
FAIL=0

chk () { # description, got, want
  if [ "$2" = "$3" ]; then
    printf '  PASS  %s (%s)\n' "$1" "$2"
    PASS=$((PASS + 1))
  else
    printf '  FAIL  %s: got "%s" want "%s"\n' "$1" "$2" "$3"
    FAIL=$((FAIL + 1))
  fi
}

chk_same () { # description, file a, file b
  if diff -q "$2" "$3" >/dev/null 2>&1; then
    printf '  PASS  %s (%s catalog/row lines identical)\n' "$1" "$(wc -l < "$2" | tr -d ' ')"
    PASS=$((PASS + 1))
  else
    printf '  FAIL  %s -- they differ:\n' "$1"
    diff -u "$2" "$3" | sed 's/^/        /'
    FAIL=$((FAIL + 1))
  fi
}

give_up () { # message
  echo
  echo "demo: cannot set up -- $1"
  echo "      see $LOGS (kept only until this script exits)"
  exit 77
}

# ------------------------------------------------------------------ cubrid ----

now_ms () { date +%s%3N; }

show_ms () { # label, elapsed ms
  printf '  %-34s %d.%03ds\n' "$1" "$(( $2 / 1000 ))" "$(( $2 % 1000 ))"
}

createdb () { # db
  CREATED_DBS="$CREATED_DBS $1"
  cubrid createdb --db-volume-size=128M --log-volume-size=64M "$1" en_US.utf8 \
    > "$LOGS/createdb.$1.log" 2>&1
}

srv_start () { # db -- start and wait until it answers a query
  cubrid server start "$1" > "$LOGS/serverstart.$1.log" 2>&1 || return 1
  local _
  for _ in $(seq 1 30); do
    csql -u dba -C -t -N -c "SELECT 1 FROM db_root" "$1" >/dev/null 2>&1 && return 0
    sleep 1
  done
  return 1
}

srv_stop () { cubrid server stop "$1" >/dev/null 2>&1; return 0; }

# Drop a database this run created, and stop tracking it so cleanup () does not
# report it as still there.
dropdb () { # db
  cubrid server stop "$1" >/dev/null 2>&1
  cubrid deletedb "$1" >/dev/null 2>&1
  CREATED_DBS="${CREATED_DBS// $1/}"
}

# Query, one whitespace-normalised row per line, blanks dropped. Client-server,
# so the database's server has to be up -- which is also why this is worth
# doing: a standalone csql pays a full server bootstrap plus a vacuum pass per
# invocation, and this script makes several dozen of these.
q () { # db, sql
  csql -u dba -C -t -N -c "$2" "$1" 2>/dev/null |
    sed -e 's/[[:space:]]\{1,\}/ /g' -e 's/^ //' -e 's/ $//' -e '/^$/d'
}

scalar () { # db, sql
  q "$1" "$2" | head -1 | tr -d ' '
}

# `csql -c` does report a failed statement in its exit status, but `csql -i
# <file>` exits 0 with a failed statement inside it -- so the file form is
# checked by grepping the log for a reported error.
stmt () { # db, sql, log
  csql -u dba -C -c "$2" "$1" > "$3" 2>&1
}

sqlfile () { # db, sql file, log, [extra csql args...]
  local db="$1" f="$2" log="$3"
  shift 3
  csql -u dba -C "$@" -i "$f" "$db" > "$log" 2>&1
  ! grep -q '^ERROR' "$log"
}

TABLES="region product audit_log customer orders"
IN_LIST="'region','product','audit_log','customer','orders'"

Q_INDEX="SELECT i.class_name, i.index_name, i.is_unique, i.is_primary_key,
                i.is_foreign_key, k.key_attr_name, k.key_order
         FROM db_index i, db_index_key k
         WHERE i.class_name = k.class_name AND i.index_name = k.index_name
           AND i.class_name IN ($IN_LIST)"
Q_ATTR="SELECT class_name, attr_name, data_type, prec, is_nullable, default_value
        FROM db_attribute WHERE class_name IN ($IN_LIST)"
# db_serial hides the serials behind an AUTO_INCREMENT column; _db_serial is
# where the round-tripped current value actually shows.
Q_SERIAL="SELECT class_name, attr_name, current_val, increment_val, cached_num
          FROM _db_serial"

# The whole observable state this demo compares: row counts, the index and
# constraint set with its key columns, the column set (which is where
# AUTO_INCREMENT shows up), and the serial's value.
state_dump () { # db, outfile
  local db="$1" t
  {
    for t in $TABLES; do
      printf 'rows   %-10s %s\n' "$t" "$(scalar "$db" "SELECT count(*) FROM $t")"
    done
    q "$db" "$Q_INDEX"  | sed 's/^/index  /' | LC_ALL=C sort
    q "$db" "$Q_ATTR"   | sed 's/^/attr   /' | LC_ALL=C sort
    q "$db" "$Q_SERIAL" | sed 's/^/serial /' | LC_ALL=C sort
  } > "$2"
}

row_counts () { # db -- the five counts on one line, for a cheap "untouched" check
  local t out=""
  for t in $TABLES; do
    out="$out $(scalar "$1" "SELECT count(*) FROM $t")"
  done
  echo "${out# }"
}

# The hand-scripted reload: schema, then the object data, then indexes. `cubrid
# loaddb` takes a single --data-file, so a default dump is three invocations and
# a --datafile-per-class dump is one per class plus two.
LOADDB_CALLS=0
loaddb_reload () { # db, dump dir, prefix, log
  local db="$1" d="$2" p="$3" log="$4" f
  LOADDB_CALLS=0
  : > "$log"
  echo "+ cubrid loaddb -S -u dba -s ${p}_schema $db" >> "$log"
  cubrid loaddb -S -u dba -s "$d/${p}_schema" "$db" >> "$log" 2>&1 || return 1
  LOADDB_CALLS=$((LOADDB_CALLS + 1))
  # Whichever object layout this dump has: the per-class glob stays unexpanded
  # when there are no such files, and the single file is absent in a per-class
  # dump, so exactly one of the two matches.
  for f in "$d/${p}_dba."*_objects "$d/${p}_objects"; do
    [ -f "$f" ] || continue
    echo "+ cubrid loaddb -S -u dba -d $(basename "$f") $db" >> "$log"
    cubrid loaddb -S -u dba -d "$f" "$db" >> "$log" 2>&1 || return 1
    LOADDB_CALLS=$((LOADDB_CALLS + 1))
  done
  echo "+ cubrid loaddb -S -u dba -i ${p}_indexes $db" >> "$log"
  cubrid loaddb -S -u dba -i "$d/${p}_indexes" "$db" >> "$log" 2>&1 || return 1
  LOADDB_CALLS=$((LOADDB_CALLS + 1))
  return 0
}

fresh_dump_copy () { # source dump dir, destination
  rm -rf "$2"
  cp -r "$1" "$2" || return 1
  rm -f "$2/importdb.manifest" "$2/importdb.exceptions"
  return 0
}

object_file_count () { # dump dir, prefix
  find "$1" -maxdepth 1 \( -name "$2_objects" -o -name "$2_dba.*_objects" \) |
    wc -l | tr -d ' '
}

# Insert rows into one class's block of a single-file object dump.
#
# The single-file layout concatenates per-class blocks, each introduced by a
# `%class [owner].[name] (col...)` header that declares the column list for the
# rows that follow. A row appended to the END of the file therefore belongs to
# whichever class happens to be last, not to the class you meant. Inserting
# immediately after the target class's own header is correct whatever the order.
inject_rows () { # objects file, class, row...
  local file="$1" cls="$2" rows
  shift 2
  rows="$(printf '%s\n' "$@")"
  awk -v cls="$cls" -v rows="$rows" '
    { print }
    !done && /^%class / {
      name = $2; sub(/.*\[/, "", name); sub(/\].*/, "", name)
      if (name == cls) { print rows; done = 1 }
    }
  ' "$file" > "$file.tmp" || return 1
  mv "$file.tmp" "$file"
}

# Which %class block does each row with one of these first-field values sit in?
# This is what proves the injection landed where it was aimed.
blocks_of_rows () { # objects file, first-field value...
  local file="$1"
  shift
  awk -v keys=" $* " '
    /^%class / { cls = $2; sub(/.*\[/, "", cls); sub(/\].*/, "", cls); next }
    index(keys, " " $1 " ") > 0 { print cls }
  ' "$file"
}

# ============================================================== stage 0 ======

banner "stage 0  build the dump an operator actually has"

echo "  install : $CUBRID"
echo "  binary  : $BIN"
if [ "$KEEP" = "1" ]; then
  echo "  scratch : $W  (KEEP -- left behind on exit)"
else
  echo "  scratch : $W  (deleted on exit)"
fi

step "creating '$SRC' and defining demo/schema.sql"
createdb "$SRC" || give_up "createdb $SRC failed (see $LOGS/createdb.$SRC.log)"
srv_start "$SRC" || give_up "cannot start the server for $SRC"
sqlfile "$SRC" "$HERE/schema.sql" "$LOGS/schema.log" \
  || give_up "schema.sql did not apply cleanly (see $LOGS/schema.log)"
chk "user classes defined in '$SRC'" \
    "$(scalar "$SRC" "SELECT count(*) FROM db_class WHERE is_system_class='NO'")" "5"

step "loading 3008 rows (demo/gen_rows.sh)"
bash "$HERE/gen_rows.sh" > "$W/rows.sql" || give_up "gen_rows.sh failed"
sqlfile "$SRC" "$W/rows.sql" "$LOGS/rows.log" --no-auto-commit \
  || give_up "row load did not apply cleanly (see $LOGS/rows.log)"
for t in $TABLES; do
  printf '     %-10s %s row(s)\n' "$t" "$(scalar "$SRC" "SELECT count(*) FROM $t")"
done

# -S is standalone, so the server has to come down before either unload.
srv_stop "$SRC"

step "cubrid unloaddb -S -u dba $SRC          <- the DEFAULT, and the demo's baseline"
( cd "$DUMP" && cubrid unloaddb -S -u dba "$SRC" ) \
  > "$LOGS/unloaddb.default.log" 2>&1 || give_up "unloaddb failed (see $LOGS/unloaddb.default.log)"
( cd "$DUMP" && ls -1 ) | sed 's/^/     /'
echo "     Three files named after the database, and ONE object file holding"
echo "     every class's rows. This is what you get if you type nothing extra."

step "cubrid unloaddb -S -u dba --datafile-per-class $SRC   <- non-default, for scenario 4"
( cd "$DUMP_PC" && cubrid unloaddb -S -u dba --datafile-per-class "$SRC" ) \
  > "$LOGS/unloaddb.perclass.log" 2>&1 || give_up "unloaddb --datafile-per-class failed"
( cd "$DUMP_PC" && ls -1 ) | sed 's/^/     /'
echo "     Same schema and indexes; the object data is split one file per class."

# The source database has served its purpose; the dumps are the input from here on.
dropdb "$SRC"

chk "object files in the default dump" "$(object_file_count "$DUMP" "$SRC")" "1"
chk "object files in the --datafile-per-class dump" "$(object_file_count "$DUMP_PC" "$SRC")" "5"
# Only the object layout differs -- worth asserting, since scenario 4 compares
# the results of importing the two and attributes any difference to the layout.
if diff -q "$DUMP/${SRC}_schema" "$DUMP_PC/${SRC}_schema" >/dev/null 2>&1; then
  chk "the two dumps carry an identical schema artifact" "identical" "identical"
else
  chk "the two dumps carry an identical schema artifact" "different" "identical"
fi

# ============================================================== stage 1 ======

banner "scenario 1  the baseline contrast (default dump)"

step "the old way: hand-scripted loaddb into '$OLD'"
createdb "$OLD" || give_up "createdb $OLD failed"
t0="$(now_ms)"
loaddb_reload "$OLD" "$DUMP" "$SRC" "$LOGS/loaddb.old.log" \
  || give_up "the hand-scripted loaddb path failed (see $LOGS/loaddb.old.log)"
old_ms=$(( $(now_ms) - t0 ))
grep '^+ cubrid loaddb' "$LOGS/loaddb.old.log" | sed 's/^+ /     /'
echo "     -> $LOADDB_CALLS invocations, and the operator supplies the order:"
echo "        -s before -d before -i, one --data-file at a time. Add"
echo "        --datafile-per-class to the dump and it is one -d per class."
srv_start "$OLD" || give_up "cannot start the server for $OLD"

step "the new way: one cubrid-importdb invocation into '$NEW'"
fresh_dump_copy "$DUMP" "$DUMP_S1" || give_up "cannot copy the dump"
createdb "$NEW" || give_up "createdb $NEW failed"
srv_start "$NEW" || give_up "cannot start the server for $NEW"
echo "     + $(basename "$BIN") -u dba $NEW <dump-dir>"
t0="$(now_ms)"
"$BIN" -u dba "$NEW" "$DUMP_S1" > "$LOGS/importdb.new.log" 2>&1
new_rc=$?
new_ms=$(( $(now_ms) - t0 ))
chk "cubrid-importdb exit status" "$new_rc" "0"
if [ "$new_rc" -ne 0 ]; then
  sed -n '1,60p' "$LOGS/importdb.new.log" | sed 's/^/        /'
fi

step "what it made of the dump"
sed -n '1,5p' "$LOGS/importdb.new.log" | sed 's/^/     /'

step "the phases it printed"
grep -E '^importdb: (defined|dependency graph|schedule|stripped|loaded|rebuilt|every FOREIGN KEY|updated statistics)' \
  "$LOGS/importdb.new.log" | sed 's/^/     /'

step "same state either way?"
state_dump "$OLD" "$W/state.old"
state_dump "$NEW" "$W/state.new"
chk "the snapshot has something in it (rows + index + column + serial lines)" \
    "$([ "$(wc -l < "$W/state.new")" -ge 30 ] && echo yes || echo no)" "yes"
chk_same "'$OLD' and '$NEW' hold the same rows, constraints, columns and serial" \
         "$W/state.old" "$W/state.new"
echo "     the constraint set both paths ended with:"
grep '^index' "$W/state.new" | sed 's/^index  /       /'
srv_stop "$OLD"
srv_stop "$NEW"

step "wall clock (NOT a benchmark)"
show_ms "hand-scripted loaddb path" "$old_ms"
show_ms "cubrid-importdb" "$new_ms"
echo "     One run of one 3008-row dump on one host, with the loaddb path in"
echo "     standalone mode and importdb against a live server. It is here so the"
echo "     transcript is honest about the cost of what you just watched -- it is"
echo "     not a measurement of either tool. Do not quote it."

# ============================================================== stage 2 ======

banner "scenario 2  the plan (--dry-run, default dump)"

fresh_dump_copy "$DUMP" "$DUMP_S2" || give_up "cannot copy the dump"
createdb "$DRY" || give_up "createdb $DRY failed"
srv_start "$DRY" || give_up "cannot start the server for $DRY"

step "$(basename "$BIN") -u dba --dry-run $DRY <dump-dir>"
"$BIN" -u dba --dry-run "$DRY" "$DUMP_S2" > "$LOGS/importdb.dry.log" 2>&1
dry_rc=$?
chk "--dry-run exit status" "$dry_rc" "0"

echo
echo "     the dependency levels it derived from the catalog:"
sed -n '/^  data phase/,/^  terminal tasks/p' "$LOGS/importdb.dry.log" |
  grep -E '^ +L[0-9]+:' | sed 's/^/    /'
echo
echo "     the terminal task order (first 8 of the list):"
grep -E '^ +#[0-9]+ ' "$LOGS/importdb.dry.log" | head -8 | sed 's/^/    /'
echo "     ... $(grep -cE '^ +#[0-9]+ ' "$LOGS/importdb.dry.log") tasks in total"
echo "     Those levels come from the CATALOG, not from the dump layout: a"
echo "     single-file dump plans exactly the same three levels."

step "and it changed nothing"
chk "levels printed" \
    "$(sed -n '/^  data phase/,/^  terminal tasks/p' "$LOGS/importdb.dry.log" | grep -cE '^ +L[0-9]+:')" "3"
# FK_DEFINE is gated on the parent's key rebuild -- the FK b-tree cannot be built
# until the parent PK it probes exists. There is no separate FK_VALIDATE task:
# the engine validates the rows while building the FK.
chk "each FK_DEFINE is gated on a prior rebuild" \
    "$(grep -E '^ +#[0-9]+ +FK_DEFINE' "$LOGS/importdb.dry.log" | grep -c 'after')" "3"
chk "user classes left in '$DRY' after the dry run" \
    "$(scalar "$DRY" "SELECT count(*) FROM db_class WHERE is_system_class='NO'")" "0"
srv_stop "$DRY"
if [ -e "$DUMP_S2/importdb.manifest" ]; then
  chk "no manifest written into the dump dir" "present" "absent"
else
  chk "no manifest written into the dump dir" "absent" "absent"
fi

# ============================================================== stage 3 ======

banner "scenario 3  a dump with orphan rows -- the one that matters"

fresh_dump_copy "$DUMP" "$DUMP_S3" || give_up "cannot copy the dump"
OBJ="$DUMP_S3/${SRC}_objects"

step "injecting two orphan 'orders' rows into the single object file"
echo "     the file's class blocks, before:"
grep -n '^%class ' "$OBJ" | sed 's/^/       /'
inject_rows "$OBJ" orders '900001 900001 1 1' '900002 900002 1 2' \
  || give_up "could not inject into $OBJ"
echo "     inserted right after the [orders] header, not at end-of-file:"
grep -n -A3 '^%class \[dba\]\.\[orders\] ' "$OBJ" | head -4 | sed 's/^/       /'
echo "     (order_id customer_id product_id qty -- customers 900001/900002 do"
echo "      not exist; product 1 does, so exactly one of the three FK edges is"
echo "      violated and the other two stay clean)"
# End-of-file would have put these rows in whichever block came last. Prove they
# are in the block that was aimed at.
chk "both injected rows sit in the [orders] block" \
    "$(blocks_of_rows "$OBJ" 900001 900002 | tr '\n' ' ' | sed 's/ $//')" \
    "orders orders"

step "the old way: hand-scripted loaddb into '$FKOLD'"
createdb "$FKOLD" || give_up "createdb $FKOLD failed"
loaddb_reload "$FKOLD" "$DUMP_S3" "$SRC" "$LOGS/loaddb.fkold.log"
fkold_rc=$?
grep -E 'object\(s\) inserted' "$LOGS/loaddb.fkold.log" | sed 's/^/     /'
chk "hand-scripted loaddb exit status on a dump it should reject" "$fkold_rc" "0"
if [ "$fkold_rc" -ne 0 ]; then
  tail -20 "$LOGS/loaddb.fkold.log" | sed 's/^/        /'
fi
srv_start "$FKOLD" || give_up "cannot start the server for $FKOLD"
chk "  ... rows in '$FKOLD'.orders (2000 + the 2 injected)" \
    "$(scalar "$FKOLD" "SELECT count(*) FROM orders")" "2002"
chk "  ... and every other table is exactly as dumped (region product audit_log customer orders)" \
    "$(row_counts "$FKOLD")" "8 200 300 500 2002"
chk "  ... fk_orders_customer present in the catalog" \
    "$(scalar "$FKOLD" "SELECT count(*) FROM db_index WHERE class_name='orders' AND index_name='fk_orders_customer'")" "1"
chk "  ... orders rows with no matching customer" \
    "$(scalar "$FKOLD" "SELECT count(*) FROM orders o WHERE NOT EXISTS (SELECT 1 FROM customer c WHERE c.customer_id = o.customer_id)")" "2"
echo "     Read those last two together: the FOREIGN KEY is in the catalog AND"
echo "     two rows violate it. loaddb turns FK checking off for the data phase"
echo "     and never turns it back on to re-check, so nothing reported anything"
echo "     and the database is now referentially broken -- quietly."

step "the new way: cubrid-importdb --continue into '$FKNEW'"
createdb "$FKNEW" || give_up "createdb $FKNEW failed"
srv_start "$FKNEW" || give_up "cannot start the server for $FKNEW"
echo "     + $(basename "$BIN") -u dba --continue $FKNEW <dump-dir>"
"$BIN" -u dba --continue "$FKNEW" "$DUMP_S3" > "$LOGS/importdb.fknew.log" 2>&1
fknew_rc=$?
grep -E '^importdb: (the engine rejected|defined [0-9]+ FK)' "$LOGS/importdb.fknew.log" | sed 's/^/     /'
sed -n '/withheld FKs/,/enumerated in/p' "$LOGS/importdb.fknew.log" | sed 's/^/     /'
chk "cubrid-importdb exit status on the same dump" "$fknew_rc" "1"
chk "  ... rows in '$FKNEW'.orders (the data is still loaded)" \
    "$(scalar "$FKNEW" "SELECT count(*) FROM orders")" "2002"
chk "  ... fk_orders_customer withheld from the catalog" \
    "$(scalar "$FKNEW" "SELECT count(*) FROM db_index WHERE class_name='orders' AND index_name='fk_orders_customer'")" "0"
chk "  ... the two clean FK edges were still defined" \
    "$(scalar "$FKNEW" "SELECT count(*) FROM db_index WHERE is_foreign_key='YES' AND class_name IN ($IN_LIST)")" "2"

step "the exceptions record it wrote"
if [ -r "$DUMP_S3/importdb.exceptions" ]; then
  sed 's/^/     /' "$DUMP_S3/importdb.exceptions"
else
  echo "     (missing)"
fi
chk "importdb.exceptions names both offending rows" \
    "$(grep -c '^orphan: ' "$DUMP_S3/importdb.exceptions" 2>/dev/null || echo 0)" "2"
chk "  ... and identifies them by primary key" \
    "$(sed -n 's/^orphan: child\[order_id=\([0-9]*\)\].*/\1/p' "$DUMP_S3/importdb.exceptions" 2>/dev/null | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')" \
    "900001 900002"

step "was withholding the FK the right call? ask the engine"
# The engine refuses to create a FOREIGN KEY over data that violates it. So the
# state the loaddb path left behind is one the engine would never have allowed
# through DDL -- which is the whole argument for re-validating.
readd="$(sed -n 's/^readd: //p' "$DUMP_S3/importdb.exceptions" 2>/dev/null | head -1)"
if [ -z "$readd" ]; then
  chk "re-add DDL recorded in importdb.exceptions" "absent" "present"
else
  echo "     $readd"
  stmt "$FKNEW" "$readd" "$LOGS/readd.reject.log"
  readd_rc=$?
  grep -E '^ERROR' "$LOGS/readd.reject.log" | sed 's/^/     /'
  chk "the engine rejects that FK while the orphans are there" \
      "$([ "$readd_rc" -ne 0 ] && echo rejected || echo accepted)" "rejected"

  step "repair the two rows the record names, then apply the recorded DDL"
  keys="$(sed -n 's/^orphan: child\[order_id=\([0-9]*\)\].*/\1/p' "$DUMP_S3/importdb.exceptions" |
            LC_ALL=C sort | tr '\n' ',' | sed 's/,$//')"
  echo "     DELETE FROM orders WHERE order_id IN ($keys);"
  stmt "$FKNEW" "DELETE FROM orders WHERE order_id IN ($keys)" "$LOGS/repair.log"
  stmt "$FKNEW" "$readd" "$LOGS/readd.accept.log"
  chk "fk_orders_customer now defined after the repair" \
      "$(scalar "$FKNEW" "SELECT count(*) FROM db_index WHERE class_name='orders' AND index_name='fk_orders_customer'")" "1"
  chk "  ... and '$FKNEW' matches the source row count again" \
      "$(scalar "$FKNEW" "SELECT count(*) FROM orders")" "2000"
fi
srv_stop "$FKOLD"
srv_stop "$FKNEW"

# ============================================================== stage 4 ======

banner "scenario 4  --degree=4 twice: the dump layout is what buys parallelism"

echo "  --degree=N fans the data phase out over OBJECT FILES, through a bounded"
echo "  pool of \`cub_admin loaddb -C\` children. The degree is clamped to the"
echo "  number of files, so the same flag does two different things depending on"
echo "  how the dump was taken."

step "4a  --degree=4 on the DEFAULT dump (one object file)"
fresh_dump_copy "$DUMP" "$DUMP_S4A" || give_up "cannot copy the dump"
createdb "$PARD" || give_up "createdb $PARD failed"
srv_start "$PARD" || give_up "cannot start the server for $PARD"
echo "     + $(basename "$BIN") -u dba --degree=4 $PARD <default dump>"
"$BIN" -u dba --degree=4 "$PARD" "$DUMP_S4A" > "$LOGS/importdb.par.default.log" 2>&1
pard_rc=$?
grep -E '^ +objects: ' "$LOGS/importdb.par.default.log" | sed 's/^/     /'
grep -E '^importdb: loaded ' "$LOGS/importdb.par.default.log" | sed 's/^/     /'
chk "--degree=4 on the default dump: exit status" "$pard_rc" "0"
chk "  ... object files available to fan out over" \
    "$(object_file_count "$DUMP_S4A" "$SRC")" "1"
chk "  ... no 'at degree' line: it clamped to serial" \
    "$(grep -c 'at degree' "$LOGS/importdb.par.default.log")" "0"
srv_stop "$PARD"
echo "     The flag was accepted and did nothing. With one object file there is"
echo "     one child to spawn, so the degree collapses to 1 and the data phase"
echo "     reports the plain serial line. Nothing warns you about this."

step "4b  --degree=4 on the --datafile-per-class dump (five object files)"
fresh_dump_copy "$DUMP_PC" "$DUMP_S4B" || give_up "cannot copy the dump"
createdb "$PARP" || give_up "createdb $PARP failed"
srv_start "$PARP" || give_up "cannot start the server for $PARP"
echo "     + $(basename "$BIN") -u dba --degree=4 $PARP <per-class dump>"
t0="$(now_ms)"
"$BIN" -u dba --degree=4 "$PARP" "$DUMP_S4B" > "$LOGS/importdb.par.perclass.log" 2>&1
parp_rc=$?
parp_ms=$(( $(now_ms) - t0 ))
grep -E '^ +objects: ' "$LOGS/importdb.par.perclass.log" | sed 's/^/     /'
grep -E '^importdb: (loaded|rebuilt|every FOREIGN KEY|defined [0-9]+ FK|updated statistics)' \
  "$LOGS/importdb.par.perclass.log" | sed 's/^/     /'
chk "--degree=4 on the per-class dump: exit status" "$parp_rc" "0"
chk "  ... object files available to fan out over" \
    "$(object_file_count "$DUMP_S4B" "$SRC")" "5"
chk "  ... the data phase reports the degree it used" \
    "$(grep -c 'at degree 4 (inter-table parallel' "$LOGS/importdb.par.perclass.log")" "1"

step "and the result is the same either way"
state_dump "$PARP" "$W/state.par"
chk_same "'$PARP' (degree 4, per-class) and '$NEW' (serial, default dump) hold the same state" \
         "$W/state.new" "$W/state.par"
srv_stop "$PARP"
show_ms "4b wall clock (not a benchmark)" "$parp_ms"
echo "     Five object files and three thousand rows is far below the point"
echo "     where fan-out shows up as time, so no speedup is claimed. What the"
echo "     two halves of this scenario assert is that --degree needs"
echo "     'unloaddb --datafile-per-class' to do anything at all, and that when"
echo "     it does, the result does not change."

# ================================================================ verdict ====

banner "verdict"
printf '  %d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -eq 0 ]; then
  echo "  demo: PASS"
  exit 0
fi
echo "  demo: FAIL"
exit 1
