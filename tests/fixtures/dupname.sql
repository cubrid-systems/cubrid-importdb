-- dupname.sql -- the same constraint name on more than one class.
--
-- CUBRID index names are unique per class, not per database, so [pk1] and [u1]
-- may each sit on several classes at once. The Rebuild phase matches the dump's
-- ADD CONSTRAINT statements against the set the Strip phase recorded, and keying
-- that set by name alone made every statement for a repeated name resolve to
-- whichever class was recorded last. The FK phase keyed its edges the same
-- way, and there it dropped the second statement as already processed -- so one
-- of the two FOREIGN KEYs was never defined at all.

CREATE TABLE dn_a (id INTEGER, code VARCHAR(10),
                   CONSTRAINT pk1 PRIMARY KEY(id), CONSTRAINT u1 UNIQUE(code));
CREATE TABLE dn_b (id INTEGER, code VARCHAR(10),
                   CONSTRAINT pk1 PRIMARY KEY(id), CONSTRAINT u1 UNIQUE(code));
CREATE TABLE dn_c (aid INTEGER, CONSTRAINT pk1 PRIMARY KEY(aid),
                   CONSTRAINT fk1 FOREIGN KEY(aid) REFERENCES dn_a(id));
CREATE TABLE dn_d (bid INTEGER, CONSTRAINT pk1 PRIMARY KEY(bid),
                   CONSTRAINT fk1 FOREIGN KEY(bid) REFERENCES dn_b(id));

-- The class name is the other half of the identity the matcher has to read out of
-- the dump's text, and a delimited name may contain the ADD keyword the matcher
-- looks for. This one is why statement_class () scans with bracket depth instead
-- of searching for " ADD ": a plain search finds the one inside the name and
-- resolves the owner, [dba], as the class.
CREATE TABLE "dn add e" (id INTEGER, CONSTRAINT pk1 PRIMARY KEY(id));
