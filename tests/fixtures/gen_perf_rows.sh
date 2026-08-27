#!/bin/bash
# gen_perf_rows.sh [fact-rows] [side-rows] [dims] -- emit the INSERT statements
# for the perf fixture's schema on stdout.
#
# Separate from gen_rows.sh because the perf fixture is the only one whose row
# volume is measured in hundreds of thousands: the generator is parameterised on
# all three table sizes so the case can trade fixture size against wall clock
# without touching the schema.
#
# Everything is deterministic, so the source, the loaddb arm and the importdb
# arm can be compared byte for byte.
#
# The output is one transaction ending in COMMIT, meant for
# `csql --no-auto-commit -i`.
set -uo pipefail

facts=${1:-160000}
sides=${2:-30000}
dims=${3:-2000}

awk -v facts="$facts" -v sides="$sides" -v dims="$dims" 'BEGIN {
  batch = 2000;

  for (s = 1; s <= dims; s += batch) {
    e = s + batch - 1;
    if (e > dims) e = dims;
    printf "INSERT INTO pf_dim VALUES ";
    for (i = s; i <= e; i++) {
      if (i > s) printf ",";
      printf "(%d,\047C%06d\047,\047dim %d\047)", i, i, i;
    }
    printf ";\n";
  }

  for (s = 1; s <= facts; s += batch) {
    e = s + batch - 1;
    if (e > facts) e = facts;
    printf "INSERT INTO pf_fact VALUES ";
    for (i = s; i <= e; i++) {
      if (i > s) printf ",";
      printf "(%d,%d,\047K%010d\047,%d,%d.75,\047pad-%08d-xxxxxxxxxxxxxxxx\047)", \
             i, (i % dims) + 1, i, i % 1000, i % 100000, i;
    }
    printf ";\n";
  }

  for (t = 1; t <= 3; t++) {
    for (s = 1; s <= sides; s += batch) {
      e = s + batch - 1;
      if (e > sides) e = sides;
      printf "INSERT INTO pf_side%d VALUES ", t;
      for (i = s; i <= e; i++) {
        if (i > s) printf ",";
        printf "(%d,%d,\047side-%d-%08d\047)", i, (i % dims) + 1, t, i;
      }
      printf ";\n";
    }
  }

  print "COMMIT;";
}'
