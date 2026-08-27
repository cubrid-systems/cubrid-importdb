-- fkcycle.sql -- two tables that reference each other.
--
-- cy_a.bid -> cy_b.bid and cy_b.aid -> cy_a.aid. No ordering of the two loads
-- satisfies both foreign keys, which is why importdb strips the constraints
-- before the data phase and defines them again afterwards.

CREATE TABLE cy_a (aid INTEGER PRIMARY KEY, bid INTEGER, nm VARCHAR(20));
CREATE TABLE cy_b (bid INTEGER PRIMARY KEY, aid INTEGER, nm VARCHAR(20));
ALTER TABLE cy_a ADD CONSTRAINT fk_a_b FOREIGN KEY (bid) REFERENCES cy_b(bid);
ALTER TABLE cy_b ADD CONSTRAINT fk_b_a FOREIGN KEY (aid) REFERENCES cy_a(aid);
