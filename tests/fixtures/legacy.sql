-- legacy.sql -- the surface a VERSION UPGRADE has to carry, over and above the
-- column types in types.sql. The crossversion case applies both to one source
-- database, so a single dump covers the types and this.
--
-- Every item here is present because an old dump writes it in a shape the
-- current engine no longer takes for granted:
--
--   lg_seq       an explicitly named SERIAL. A pre-11.5 unloaddb follows it with
--                `call [change_serial_owner](..) on class [db_serial];`, and in
--                11.5 db_serial is a VIEW that does not carry the method. This
--                one statement is why an unpatched importdb refuses every 10.2
--                dump of a database that owns a serial -- see
--                rewrite_pre115_call_targets () in src/import_define.cpp.
--   lg_reader /  two users and their grants. unloaddb writes them into the
--   lg_writer    schema file as `call [add_user](..)` plus GRANT statements, so
--                an import either brings the accounts and privileges back or
--                silently leaves the database unusable by its applications
--                (OFFICE-656 1-1 checks exactly this).
--   lg_parent /  a foreign key, so the cross-version dump also exercises the
--   lg_child     strip / rebuild / FK-define lifecycle rather than PKs alone.
--   lg_v_child   a view. Old unloaddb emits `CREATE VCLASS` early and
--                `ALTER VCLASS .. ADD QUERY` later; the query spec is the half
--                that has broken before (RND-2774).
--   lg_trg       a trigger, which importdb defines strictly last.

CREATE USER lg_reader;
CREATE USER lg_writer PASSWORD 'lgpw';

CREATE SERIAL lg_seq START WITH 100 INCREMENT BY 5 MAXVALUE 1000000;

CREATE TABLE lg_parent (
  pid  INTEGER PRIMARY KEY,
  code VARCHAR(10) UNIQUE,
  nm   VARCHAR(30) NOT NULL
);

CREATE TABLE lg_child (
  cid    INTEGER PRIMARY KEY,
  pid    INTEGER,
  amount NUMERIC(12,2) DEFAULT 0,
  CONSTRAINT fk_lg_child_parent FOREIGN KEY (pid) REFERENCES lg_parent(pid)
);

CREATE INDEX i_lg_child_amount ON lg_child(amount DESC);

CREATE VCLASS lg_v_child AS
  SELECT c.cid, c.pid, p.nm, c.amount FROM lg_child c, lg_parent p WHERE c.pid = p.pid;

CREATE TRIGGER lg_trg BEFORE INSERT ON lg_parent EXECUTE PRINT 'lg_parent insert';

GRANT SELECT ON lg_parent TO lg_reader;
GRANT SELECT ON lg_child  TO lg_reader;
GRANT SELECT, INSERT, UPDATE, DELETE ON lg_child TO lg_writer;

-- advance the serial so its current value is not its start value
SELECT lg_seq.next_value;
SELECT lg_seq.next_value;
