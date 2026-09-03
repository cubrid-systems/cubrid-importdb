-- dupname.sql -- the same constraint name on more than one class.
--
-- CUBRID index names are unique per class, not per database, so [pk1] and [u1]
-- may each sit on several classes at once. The Rebuild phase matches the dump's
-- ADD CONSTRAINT statements against the set the Strip phase recorded, and keying
-- that set by name alone made every statement for a repeated name resolve to
-- whichever class was recorded last.

CREATE TABLE dn_a (id INTEGER, code VARCHAR(10),
                   CONSTRAINT pk1 PRIMARY KEY(id), CONSTRAINT u1 UNIQUE(code));
CREATE TABLE dn_b (id INTEGER, code VARCHAR(10),
                   CONSTRAINT pk1 PRIMARY KEY(id), CONSTRAINT u1 UNIQUE(code));
CREATE TABLE dn_c (aid INTEGER, CONSTRAINT pk1 PRIMARY KEY(aid),
                   CONSTRAINT fk_c_a FOREIGN KEY(aid) REFERENCES dn_a(id));
