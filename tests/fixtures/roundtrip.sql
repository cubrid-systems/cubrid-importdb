-- roundtrip.sql -- the fidelity fixture.
--
-- Everything the round-trip claim has to carry across: a PK, a single-column
-- UNIQUE, a NOT NULL, a DEFAULT, a foreign key, a plain index, a composite
-- index with a DESC key, a multi-column UNIQUE index, a REVERSE index, a table
-- with no key at all, a serial whose current value has already advanced, and a
-- trigger (defined strictly last by the import, so it must not fire on the
-- bulk-loaded rows). The column types cover the families unloaddb formats
-- differently: integer, fixed and varying character, date, time, datetime,
-- timestamp, exact numeric and float.

CREATE TABLE rt_dept (
  dept_id INTEGER PRIMARY KEY,
  code    VARCHAR(8) UNIQUE,
  nm      VARCHAR(40) NOT NULL,
  budget  NUMERIC(12,2) DEFAULT 0
);

CREATE TABLE rt_emp (
  emp_id  INTEGER PRIMARY KEY,
  dept_id INTEGER,
  nm      VARCHAR(40),
  grade   CHAR(2),
  hired   DATE,
  shiftat TIME,
  updated DATETIME,
  stamped TIMESTAMP,
  salary  NUMERIC(10,2),
  ratio   DOUBLE,
  small_v SMALLINT,
  big_v   BIGINT,
  memo    VARCHAR(200),
  CONSTRAINT fk_emp_dept FOREIGN KEY (dept_id) REFERENCES rt_dept(dept_id)
);

CREATE TABLE rt_note (note_id INTEGER, body VARCHAR(60));

CREATE INDEX i_emp_nm ON rt_emp(nm);
CREATE INDEX i_emp_dept_hired ON rt_emp(dept_id, hired DESC);
CREATE UNIQUE INDEX u_emp_id_nm ON rt_emp(emp_id, nm);
CREATE REVERSE INDEX ri_emp_salary ON rt_emp(salary);

CREATE SERIAL rt_seq START WITH 5 INCREMENT BY 3 MAXVALUE 100000;

CREATE TRIGGER trg_dept BEFORE INSERT ON rt_dept EXECUTE PRINT 'dept insert';

-- advance the serial so its current value is not its start value: an import
-- that recreated the serial from scratch would lose this.
SELECT rt_seq.next_value;
SELECT rt_seq.next_value;
