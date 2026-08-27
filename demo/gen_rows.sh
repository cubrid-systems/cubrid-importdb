#!/bin/bash
# demo/gen_rows.sh -- emit the demo's INSERT statements on stdout.
#
# 3008 rows across the five tables of demo/schema.sql, generated rather than
# checked in so the file stays readable:
#
#     region       8   product  200   audit_log  300
#     customer   500   orders  2000
#
# Deterministic: every FK value points at a row that exists, and every UNIQUE
# column is distinct. Statements are chunked so no single INSERT gets absurd.
set -uo pipefail

gen_region () {
  local i
  printf 'INSERT INTO region VALUES '
  for i in $(seq 1 8); do
    [ "$i" -gt 1 ] && printf ','
    printf "(%d,'R%02d','Region %02d')" "$i" "$i" "$i"
  done
  printf ';\n'
}

gen_product () {
  local i first=1
  for i in $(seq 1 200); do
    if [ "$first" -eq 1 ]; then printf 'INSERT INTO product VALUES '; first=0; else printf ','; fi
    printf "(%d,'SKU-%05d',%d)" "$i" "$i" "$(( 100 + (i * 7) % 900 ))"
    if [ "$(( i % 100 ))" -eq 0 ]; then printf ';\n'; first=1; fi
  done
}

# entry_id is AUTO_INCREMENT, so the column list omits it on purpose.
gen_audit_log () {
  local i first=1
  for i in $(seq 1 300); do
    if [ "$first" -eq 1 ]; then printf 'INSERT INTO audit_log (note) VALUES '; first=0; else printf ','; fi
    printf "('seed note %d')" "$i"
    if [ "$(( i % 100 ))" -eq 0 ]; then printf ';\n'; first=1; fi
  done
}

gen_customer () {
  local i first=1
  for i in $(seq 1 500); do
    if [ "$first" -eq 1 ]; then printf 'INSERT INTO customer VALUES '; first=0; else printf ','; fi
    printf "(%d,%d,'user%04d@example.com','City%02d')" "$i" "$(( (i % 8) + 1 ))" "$i" "$(( i % 20 ))"
    if [ "$(( i % 250 ))" -eq 0 ]; then printf ';\n'; first=1; fi
  done
}

gen_orders () {
  local i first=1
  for i in $(seq 1 2000); do
    if [ "$first" -eq 1 ]; then printf 'INSERT INTO orders VALUES '; first=0; else printf ','; fi
    printf '(%d,%d,%d,%d)' "$i" "$(( (i % 500) + 1 ))" "$(( (i % 200) + 1 ))" "$(( (i % 9) + 1 ))"
    if [ "$(( i % 500 ))" -eq 0 ]; then printf ';\n'; first=1; fi
  done
}

gen_region
gen_product
gen_audit_log
gen_customer
gen_orders
printf 'COMMIT;\n'
