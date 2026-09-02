# What importdb prints

Everything the tool says, in full. The README carries the argument; this carries
the output, because a page of verbatim console text is reference material and
reads like padding next to the argument it is meant to support.

## A complete run

Each phase announces what it did. A complete run, verbatim:

```
importdb: rostered default dump (prefix 'shop') from /tmp/rmcap/dump
    schema:   shop_schema (single)
    objects:  single (shop_objects)
    indexes:  shop_indexes
    triggers: (none)
importdb: defined 'shoptgt'.
importdb: dependency graph for 'shoptgt' -- 5 node(s), 3 FK edge(s), 0 inheritance edge(s), 1 serial(s)
  - audit_log: pk=pk_audit_log uk=[] fk=[]   [serials=[audit_log_ai_entry_id]]
  - customer: pk=pk_customer uk=[uk_customer_email] fk=[fk_customer_region]
  - orders: pk=pk_orders uk=[] fk=[fk_orders_customer, fk_orders_product]
  - product: pk=pk_product uk=[uk_product_sku] fk=[]
  - region: pk=pk_region uk=[uk_region_code] fk=[]
  FK edges (child -> parent):
      customer -> region   (fk_customer_region)
      orders -> customer   (fk_orders_customer)
      orders -> product   (fk_orders_product)
  cycles: none
  level sets (parallel-eligible; complete=true):
      L0: [audit_log, product, region]
      L1: [customer]
      L2: [orders]
importdb: schedule for 'shoptgt' -- data phase 3 level(s), 17 terminal task(s)
  data phase (parallel-eligible level sets):
      L0: [audit_log, product, region]
      L1: [customer]
      L2: [orders]
  terminal tasks (in a valid execution order):
      #0  REBUILD_PK      audit_log (pk_audit_log)
      #1  REBUILD_PK      customer (pk_customer)
      #2  REBUILD_PK      orders (pk_orders)
      #3  REBUILD_PK      product (pk_product)
      #4  REBUILD_PK      region (pk_region)
      #5  REBUILD_UNIQUE  customer (uk_customer_email)
      #6  REBUILD_UNIQUE  product (uk_product_sku)
      #7  REBUILD_UNIQUE  region (uk_region_code)
      #8  BUILD_INDEX     <deferred indexes: shop_indexes>
      #9  FK_DEFINE       customer -> region (fk_customer_region)   [after #4, #7]
      #10 FK_DEFINE       orders -> customer (fk_orders_customer)   [after #1, #5]
      #11 FK_DEFINE       orders -> product (fk_orders_product)   [after #3, #6]
      #12 STATS           audit_log   [after #0, #8]
      #13 STATS           customer   [after #1, #5, #8]
      #14 STATS           orders   [after #2, #8]
      #15 STATS           product   [after #3, #6, #8]
      #16 STATS           region   [after #4, #7, #8]
importdb: stripped 11 constraint(s) from 'shoptgt'.
importdb: loaded 3008 row(s) from 1 object file(s) into 'shoptgt'.
importdb: rebuilt 8 constraint(s) and built 2 index(es) on 'shoptgt'.
importdb: every FOREIGN KEY on 'shoptgt' was accepted by the engine -- 3 edge(s) clean, 0 skipped (parent key withheld).
importdb: defined 3 FK(s) on 'shoptgt'.
importdb: updated statistics on 5 class(es) in 'shoptgt'.
```


## Knowing it worked

The run ends with a consolidated report — the per-class verdict, the row counts,
and anything left for you to repair. It is the answer to "did that actually
work?", and it is the same record `importdb.manifest` carries:

```
importdb: ===== import report: 'shoptgt' -- COMPLETE =====
  planned data order (3 level(s)):
      L0: [audit_log, product, region]
      L1: [customer]
      L2: [orders]
  classes (5 imported, 0 skipped):
      done     audit_log
      done     customer
      done     orders
      done     product
      done     region
  loaded: 3008 row(s) from 1 object file(s)
      shop_objects: 3008 row(s)
importdb: 'shoptgt' import COMPLETE -- 5 class(es) done, 0 skipped, 0 pending, 0 FK(s) withheld; 3008 row(s) loaded. Repair records + re-add DDL in /tmp/rmcap/dump/importdb.manifest.
```

## How the passwords travel

`unloaddb` writes an `add_user` call, a `set_password_encoded_sha1` call and the
`GRANT` statements into `<prefix>_schema`, so users and their credentials come
back with everything else. The password is easy to believe lost, because the
`add_user` line carries an empty one:

```
call [add_user]('LG_WRITER', '') on class [db_root] to [auser];
call [set_password_encoded_sha1]('DE05ABA8...CAAA') on [auser];
```

The hash arrives in the *next* statement. Measured both ways in
`tests/run_tests.sh crossversion`, which logs in as the restored account with the
password set on the old engine, reads what it was granted, and checks that a
wrong password is still refused.


## The live display

On a terminal, the phase lines above scroll past under a block that is redrawn in
place. It answers what the printed lines cannot: which phase is running, how many
are left, and how far into it you are.

```
 importdb  tuitgt                                           load  [6/10]  00:00
  ███████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  23%  0/4 done · 2 loading
    tuisrc_dba.customer         ██████████████████████████████░░░░░  85%  2 MB
    tuisrc_dba.orders           ██████░░░░░░░░░░░░░░░░░░░░░░░░░░░░  17%  18 MB
```

*(A 400,050-row import at `--degree=2`, caught a fraction of a second in — the
elapsed clock is `MM:SS`.)*

The per-file bars are real, not estimated. The loaders are separate
`cub_admin loaddb -C` processes that report nothing until they exit, so importdb
reads each one's file offset out of `/proc/<pid>/fdinfo` — that is the only honest
progress signal a child of that shape has. It tracks the *read* of the object
file, which leads the commit of its rows, so a file sits at 100% for as long as
its last transaction takes.

The display is **off whenever stdout is not a terminal**, so a pipe, a file, a
`cron` job or a CI log gets exactly the plain output it always got. `--progress`
forces the question either way; `NO_COLOR` is honoured.


## The exceptions artifact

When the engine rejects a foreign key, importdb writes every offending row to
`importdb.exceptions` in the dump directory:

```
# CUBRID importdb FK re-validation exceptions (format v1)
# Written and owned by importdb; do not edit by hand.
# Each 'orphan' line is a child row whose foreign-key value has no matching parent primary key.
# An operator repairs the data from this file; the FK on these edges is left undefined.
generator: importdb
created: 2026-08-28T02:20:49Z
database: shopbad
policy: fail-fast
violated_edges: 1
total_orphans: 2

[edge] orders -> customer (fk_orders_customer)
child_key_columns: order_id
fk_columns: customer_id
orphans: 2
orphan: child[order_id=900001] fk[customer_id=777]
orphan: child[order_id=900002] fk[customer_id=888]

# CUBRID importdb FK define: the following FK(s) were NOT defined (withheld).
# An operator repairs the referenced data, then re-runs each 'readd' statement to add the FK.
[fk-withheld] orders -> customer (fk_orders_customer) :: ADD FOREIGN KEY rejected by the engine: 2 orphan row(s)
readd: ALTER CLASS [dba].[orders] ADD CONSTRAINT [fk_orders_customer] FOREIGN KEY([customer_id]) WITH DEDUPLICATE=0 REFERENCES [dba].[customer] ON DELETE RESTRICT ON UPDATE RESTRICT;
[fk-withheld] orders -> product (fk_orders_product) :: not attempted (fail-fast stopped at an earlier rejected edge)
readd: ALTER CLASS [dba].[orders] ADD CONSTRAINT [fk_orders_product] FOREIGN KEY([product_id]) WITH DEDUPLICATE=0 REFERENCES [dba].[product] ON DELETE RESTRICT ON UPDATE RESTRICT;
```

The engine names only the first offending value and stops; importdb enumerates all
of them. Fix the data, run the `readd:` statement, and the constraint set matches
the dump. `--continue` attempts every edge instead of stopping at the first.


## Where these come from, and how far to trust them

`demo/run_demo.sh` produces this output -- the complete run in its first scenario,
the violation in its third -- and prints the phase lines under a `the phases it
printed` step. It does not compare them to anything: what the demo and the suite
assert is the facts around the text, the exit status, the counts, and specific
strings like `at degree 4.`, not the paragraphs.

So this page can drift from the tool without anything failing. If it disagrees
with what your build prints, the build is right. Run the demo and read its own
output rather than trusting a transcript.
