-- wide.sql -- one parent and four children, used by the parallelism and resume
-- cases.
--
-- Five object files give the bounded load pool something to bound: --degree=4
-- can run four children at once, --degree=1 runs them one at a time, and the
-- final state must be the same either way. The row volume is a parameter of
-- gen_rows.sh, so the resume case can make the data phase long enough to be
-- interrupted inside it while the parallelism case stays cheap.

CREATE TABLE wd_p (id INTEGER PRIMARY KEY, nm VARCHAR(20));
CREATE TABLE wd_c1 (cid INTEGER PRIMARY KEY, pid INTEGER, pad VARCHAR(60),
  CONSTRAINT fk_wd_c1 FOREIGN KEY (pid) REFERENCES wd_p(id));
CREATE TABLE wd_c2 (cid INTEGER PRIMARY KEY, pid INTEGER, pad VARCHAR(60),
  CONSTRAINT fk_wd_c2 FOREIGN KEY (pid) REFERENCES wd_p(id));
CREATE TABLE wd_c3 (cid INTEGER PRIMARY KEY, pid INTEGER, pad VARCHAR(60),
  CONSTRAINT fk_wd_c3 FOREIGN KEY (pid) REFERENCES wd_p(id));
CREATE TABLE wd_c4 (cid INTEGER PRIMARY KEY, pid INTEGER, pad VARCHAR(60),
  CONSTRAINT fk_wd_c4 FOREIGN KEY (pid) REFERENCES wd_p(id));
CREATE INDEX i_wd_c1_pad ON wd_c1(pad);
