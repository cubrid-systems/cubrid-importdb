-- types.sql -- the data-type fixture.
--
-- One table per family unloaddb formats differently, so a failure names the
-- family instead of the fixture. Everything here is valid DDL on 10.2 as well
-- as on 11.5: this fixture is what the crossversion case builds on the OLD
-- engine, so a statement only 11.x accepts cannot go in it.
--
-- What each table is for:
--   ty_num   exact and approximate numerics at their limits, and MONETARY
--   ty_str   fixed/varying, NCHAR, two collations in one row, and non-ASCII
--   ty_bit   BIT and BIT VARYING, which unloaddb writes as X'..' literals
--   ty_time  every date/time type INCLUDING the four zoned ones (10.0+)
--   ty_enum  ENUM -- the family OFFICE-656 1-2 names and the suite had missed
--   ty_json  JSON -- object and empty array
--   ty_lob   BLOB and CLOB, which live outside the object file as ELO refs
--   ty_coll  SET / MULTISET / SEQUENCE of primitives -- value-typed, so a
--            CS-mode load must carry them (an OBJECT column would be refused)
--   ty_wide  one value larger than unloaddb's internal buffer (CBRD-26282,
--            where a 978 KB VARCHAR came back corrupted)
--   ty_ai    AUTO_INCREMENT, whose serial unloaddb restores with ALTER SERIAL
--   ty_cmt   COMMENT on the table and on a column
--   ty_hash  HASH partitioning   (the suite had only RANGE)
--   ty_list  LIST partitioning   (ditto)
--   ty_fidx  a filtered index and a function index

CREATE TABLE ty_num (
  id       INTEGER PRIMARY KEY,
  si       SMALLINT,
  bi       BIGINT,
  n_exact  NUMERIC(20,6),
  n_big    NUMERIC(38,0),
  f4       FLOAT,
  f8       DOUBLE,
  mon      MONETARY
);

CREATE TABLE ty_str (
  id   INTEGER PRIMARY KEY,
  c    CHAR(5),
  vc   VARCHAR(40),
  nc   NCHAR(4),
  nvc  NCHAR VARYING(20),
  vb   VARCHAR(30) COLLATE utf8_bin,
  vi   VARCHAR(30) COLLATE iso88591_bin,
  ko   VARCHAR(60)
);

CREATE TABLE ty_bit (id INTEGER PRIMARY KEY, b BIT(16), bv BIT VARYING(128));

CREATE TABLE ty_time (
  id    INTEGER PRIMARY KEY,
  d     DATE,
  t     TIME,
  dt    DATETIME,
  ts    TIMESTAMP,
  tstz  TIMESTAMPTZ,
  dttz  DATETIMETZ,
  tsltz TIMESTAMPLTZ,
  dtltz DATETIMELTZ
);

CREATE TABLE ty_enum (id INTEGER PRIMARY KEY, e ENUM('small','medium','large'));

CREATE TABLE ty_json (id INTEGER PRIMARY KEY, j JSON);

CREATE TABLE ty_lob (id INTEGER PRIMARY KEY, bl BLOB, cl CLOB);

CREATE TABLE ty_coll (
  id INTEGER PRIMARY KEY,
  s  SET(INTEGER),
  m  MULTISET(VARCHAR(10)),
  q  SEQUENCE(INTEGER)
);

CREATE TABLE ty_wide (id INTEGER PRIMARY KEY, s STRING);

CREATE TABLE ty_ai (id INTEGER AUTO_INCREMENT PRIMARY KEY, v INTEGER);

CREATE TABLE ty_cmt (id INTEGER PRIMARY KEY COMMENT 'the id', v INTEGER) COMMENT 'the table';

CREATE TABLE ty_hash (id INTEGER, v INTEGER, PRIMARY KEY (id))
  PARTITION BY HASH(id) PARTITIONS 3;

CREATE TABLE ty_list (id INTEGER, r VARCHAR(2), PRIMARY KEY (id, r))
  PARTITION BY LIST(r) (PARTITION pa VALUES IN ('a'), PARTITION pb VALUES IN ('b'));

CREATE TABLE ty_fidx (id INTEGER PRIMARY KEY, v INTEGER, s VARCHAR(20));
CREATE INDEX i_ty_filtered ON ty_fidx(v) WHERE v > 10;
CREATE INDEX i_ty_func ON ty_fidx(LOWER(s));
