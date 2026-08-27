#!/bin/bash
# gen_rows.sh <fixture> [rows-per-child] -- emit the INSERT statements for a
# fixture's schema on stdout.
#
# Row data is generated rather than committed: the wide fixture alone is a few
# megabytes at the volume the resume case needs, and none of it is interesting
# to read. Everything is deterministic, so a source and its import can be
# compared byte for byte.
#
# The output is one transaction ending in COMMIT, meant for
# `csql --no-auto-commit -i`.
set -uo pipefail

fixture=${1:?usage: gen_rows.sh <fixture> [rows]}
rows=${2:-0}

case "$fixture" in
roundtrip)
  awk 'BEGIN {
    printf "INSERT INTO rt_dept VALUES ";
    for (i = 1; i <= 20; i++) {
      if (i > 1) printf ",";
      printf "(%d,\047D%03d\047,\047dept %d\047,%d.50)", i, i, i, i * 1000;
    }
    printf ";\n";
    printf "INSERT INTO rt_emp VALUES ";
    for (i = 1; i <= 200; i++) {
      if (i > 1) printf ",";
      printf "(%d,%d,\047emp %d\047,\047G%d\047,DATE\0472021-%02d-%02d\047,", \
             i, (i % 20) + 1, i, i % 10, (i % 12) + 1, (i % 28) + 1;
      printf "TIME\047%02d:%02d:00\047,DATETIME\0472022-03-%02d %02d:%02d:%02d.%03d\047,", \
             i % 24, i % 60, (i % 28) + 1, i % 24, i % 60, i % 60, i % 1000;
      printf "TIMESTAMP\0472023-04-%02d %02d:%02d:%02d\047,", (i % 28) + 1, i % 24, i % 60, i % 60;
      printf "%d.25,%d.125,%d,%d,\047memo-%d\047)", i * 13, i % 97, (i % 300) - 150, i * 100000, i;
    }
    printf ";\n";
    # one all-NULL row, so NULL formatting is part of the checksum
    print "INSERT INTO rt_emp VALUES (999,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);";
    printf "INSERT INTO rt_note VALUES ";
    for (i = 1; i <= 30; i++) {
      if (i > 1) printf ",";
      printf "(%d,\047note %d\047)", i, i;
    }
    printf ";\n";
    print "COMMIT;";
  }'
  ;;
ordering)
  awk 'BEGIN {
    printf "INSERT INTO lvl0 VALUES ";
    for (i = 1; i <= 10; i++) { if (i > 1) printf ","; printf "(%d,\047l0-%d\047)", i, i; }
    printf ";\n";
    printf "INSERT INTO lvl1 VALUES ";
    for (i = 1; i <= 20; i++) { if (i > 1) printf ","; printf "(%d,%d)", i, (i % 10) + 1; }
    printf ";\n";
    printf "INSERT INTO lvl2 VALUES ";
    for (i = 1; i <= 30; i++) { if (i > 1) printf ","; printf "(%d,%d)", i, (i % 20) + 1; }
    printf ";\n";
    printf "INSERT INTO lvl3 VALUES ";
    for (i = 1; i <= 40; i++) { if (i > 1) printf ","; printf "(%d,%d)", i, (i % 30) + 1; }
    printf ";\n";
    printf "INSERT INTO base_t VALUES ";
    for (i = 1; i <= 10; i++) { if (i > 1) printf ","; printf "(%d,\047b-%d\047)", i, i; }
    printf ";\n";
    printf "INSERT INTO deriv_t VALUES ";
    for (i = 101; i <= 115; i++) { if (i > 101) printf ","; printf "(%d,\047d-%d\047,%d)", i, i, i * 2; }
    printf ";\n";
    printf "INSERT INTO part_t VALUES ";
    for (i = 1; i <= 45; i++) { if (i > 1) printf ","; printf "(%d,%d,\047v%d\047)", i, (i % 30), i; }
    printf ";\n";
    print "COMMIT;";
  }'
  ;;
fkcycle)
  # the cycle cannot be populated in one pass either: insert the a-side with a
  # NULL reference, insert the b-side, then close the loop with UPDATEs.
  awk 'BEGIN {
    n = 20;
    printf "INSERT INTO cy_a VALUES ";
    for (i = 1; i <= n; i++) { if (i > 1) printf ","; printf "(%d,NULL,\047a%d\047)", i, i; }
    printf ";\n";
    printf "INSERT INTO cy_b VALUES ";
    for (i = 1; i <= n; i++) { if (i > 1) printf ","; printf "(%d,%d,\047b%d\047)", i, i, i; }
    printf ";\n";
    for (i = 1; i <= n; i++) { printf "UPDATE cy_a SET bid=%d WHERE aid=%d;\n", i, i; }
    print "COMMIT;";
  }'
  ;;
fkviolation)
  awk 'BEGIN {
    printf "INSERT INTO fv_p VALUES ";
    for (i = 1; i <= 10; i++) { if (i > 1) printf ","; printf "(%d,\047p%d\047)", i, i; }
    printf ";\n";
    printf "INSERT INTO fv_c1 VALUES ";
    for (i = 1; i <= 20; i++) { if (i > 1) printf ","; printf "(%d,%d,%d)", i, (i % 10) + 1, i * 10; }
    printf ";\n";
    printf "INSERT INTO fv_c2 VALUES ";
    for (i = 1; i <= 15; i++) { if (i > 1) printf ","; printf "(%d,%d,%d)", i, (i % 10) + 1, i * 10; }
    printf ";\n";
    print "COMMIT;";
  }'
  ;;
wide)
  [ "$rows" -gt 0 ] || rows=4000
  awk -v rows="$rows" 'BEGIN {
    parents = 200;
    batch = 2000;
    printf "INSERT INTO wd_p VALUES ";
    for (i = 1; i <= parents; i++) { if (i > 1) printf ","; printf "(%d,\047p%d\047)", i, i; }
    printf ";\n";
    for (t = 1; t <= 4; t++) {
      for (s = 1; s <= rows; s += batch) {
        e = s + batch - 1;
        if (e > rows) e = rows;
        printf "INSERT INTO wd_c%d VALUES ", t;
        for (i = s; i <= e; i++) {
          if (i > s) printf ",";
          printf "(%d,%d,\047pad-%d-%08d\047)", i, (i % parents) + 1, t, i;
        }
        printf ";\n";
      }
    }
    print "COMMIT;";
  }'
  ;;
*)
  echo "gen_rows.sh: unknown fixture '$fixture'" >&2
  exit 1
  ;;
esac
