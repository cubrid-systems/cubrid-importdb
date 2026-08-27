-- fkviolation.sql -- one parent, two children, one foreign key each.
--
-- The source is clean: CUBRID's ALTER ... ADD FOREIGN KEY does validate the
-- rows already in the table, so an orphan cannot be created here. The case
-- injects the orphan rows into the unloaddb object files instead, which is
-- exactly the shape of a dump taken from a database whose constraint was added
-- while the data was still consistent and whose rows later diverged.

CREATE TABLE fv_p (id INTEGER PRIMARY KEY, nm VARCHAR(20));
CREATE TABLE fv_c1 (cid INTEGER PRIMARY KEY, pid INTEGER, v INTEGER,
  CONSTRAINT fk_c1_p FOREIGN KEY (pid) REFERENCES fv_p(id));
CREATE TABLE fv_c2 (cid INTEGER PRIMARY KEY, pid INTEGER, v INTEGER,
  CONSTRAINT fk_c2_p FOREIGN KEY (pid) REFERENCES fv_p(id));
