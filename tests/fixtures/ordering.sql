-- ordering.sql -- the dependency-ordering fixture.
--
-- Three levels of foreign-key edges below the root (lvl0 <- lvl1 <- lvl2 <-
-- lvl3), one inheritance edge (deriv_t under base_t) and one RANGE-partitioned
-- table. The planner has to put every parent in a level strictly below its
-- child, and a subclass strictly below its superclass; the partitioned table's
-- three sub-classes are separate object files that must all load.

CREATE TABLE lvl0 (id INTEGER PRIMARY KEY, nm VARCHAR(20));
CREATE TABLE lvl1 (id INTEGER PRIMARY KEY, p0 INTEGER,
  CONSTRAINT fk_l1_l0 FOREIGN KEY (p0) REFERENCES lvl0(id));
CREATE TABLE lvl2 (id INTEGER PRIMARY KEY, p1 INTEGER,
  CONSTRAINT fk_l2_l1 FOREIGN KEY (p1) REFERENCES lvl1(id));
CREATE TABLE lvl3 (id INTEGER PRIMARY KEY, p2 INTEGER,
  CONSTRAINT fk_l3_l2 FOREIGN KEY (p2) REFERENCES lvl2(id));

CREATE TABLE base_t (bid INTEGER PRIMARY KEY, nm VARCHAR(20));
CREATE CLASS deriv_t UNDER base_t (extra INTEGER);

CREATE TABLE part_t (pid INTEGER, region INTEGER, v VARCHAR(10), PRIMARY KEY (pid, region))
  PARTITION BY RANGE (region) (
    PARTITION p0 VALUES LESS THAN (10),
    PARTITION p1 VALUES LESS THAN (20),
    PARTITION p2 VALUES LESS THAN MAXVALUE);
