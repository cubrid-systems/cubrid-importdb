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
types)
  # Values are literal rather than generated: each one is here because it is a
  # formatting edge for the family in its table, and a loop would hide that.
  # NCHAR needs an N'..' literal; a plain string is refused with "Cannot coerce".
  cat <<'ROWS'
INSERT INTO ty_num VALUES (1, -32768, 9223372036854775807, 123456789.123456, 12345678901234567890123456789012345678, 1.5, 2.25, 1234.56);
INSERT INTO ty_num VALUES (2, 32767, -9223372036854775808, -0.000001, 0, -1.5, -2.25, -0.01);
INSERT INTO ty_num VALUES (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
INSERT INTO ty_str VALUES (1, 'abcde', 'plain ascii', N'nchr', N'nchar varying', 'utf8bin', 'iso88591', '한글 문자열 테스트');
INSERT INTO ty_str VALUES (2, 'x    ', 'quote''s and \ backslash', N'a', N'b', 'c', 'd', '이모지 없는 다국어');
INSERT INTO ty_str VALUES (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
INSERT INTO ty_bit VALUES (1, B'1010101010101010', B'1111000011110000');
INSERT INTO ty_bit VALUES (2, NULL, NULL);
INSERT INTO ty_time VALUES (1, DATE'2020-02-29', TIME'23:59:59', DATETIME'2020-02-29 23:59:59.999', TIMESTAMP'2020-02-29 23:59:59', TIMESTAMPTZ'2020-02-29 23:59:59 Asia/Seoul', DATETIMETZ'2020-02-29 23:59:59.999 Asia/Seoul', TIMESTAMPLTZ'2020-02-29 23:59:59', DATETIMELTZ'2020-02-29 23:59:59.999');
INSERT INTO ty_time VALUES (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
INSERT INTO ty_enum VALUES (1, 'small'), (2, 'large'), (3, NULL);
INSERT INTO ty_json VALUES (1, '{"a": 1, "b": [1,2,3], "c": {"d": "e"}}');
INSERT INTO ty_json VALUES (2, '[]');
INSERT INTO ty_json VALUES (3, NULL);
INSERT INTO ty_lob VALUES (1, BIT_TO_BLOB(B'11110000'), CHAR_TO_CLOB('clob content here'));
INSERT INTO ty_lob VALUES (2, NULL, NULL);
INSERT INTO ty_coll VALUES (1, {1,2,3}, {'a','b','a'}, {3,1,2});
INSERT INTO ty_coll VALUES (2, {}, {}, {});
INSERT INTO ty_wide VALUES (1, RPAD('cubrid', 978670, '1234567800'));
INSERT INTO ty_wide VALUES (2, 'short');
INSERT INTO ty_ai (v) VALUES (10), (20), (30);
INSERT INTO ty_cmt VALUES (1, 1);
INSERT INTO ty_hash VALUES (1,1),(2,2),(3,3),(4,4),(5,5),(6,6);
INSERT INTO ty_list VALUES (1,'a'),(2,'b'),(3,'a'),(4,'b');
INSERT INTO ty_fidx VALUES (1, 5, 'AbC'), (2, 50, 'dEf');
COMMIT;
ROWS
  ;;
legacy)
  awk 'BEGIN {
    printf "INSERT INTO lg_parent VALUES ";
    for (i = 1; i <= 12; i++) { if (i > 1) printf ","; printf "(%d,\047C%04d\047,\047parent %d\047)", i, i, i; }
    printf ";\n";
    printf "INSERT INTO lg_child VALUES ";
    for (i = 1; i <= 40; i++) { if (i > 1) printf ","; printf "(%d,%d,%d.75)", i, (i % 12) + 1, i * 100; }
    printf ";\n";
    print "INSERT INTO lg_child VALUES (999, NULL, NULL);";
    print "COMMIT;";
  }'
  ;;
*)
  echo "gen_rows.sh: unknown fixture '$fixture'" >&2
  exit 1
  ;;
esac
