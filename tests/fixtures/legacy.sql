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
--   lg_tree /    the view shape that actually broke. A pre-11.5 unloaddb writes a
--   lg_v_tree    view's query specs with the SELECT list replaced by NA, and RND-2774
--                is what happens next: a subquery whose list is NA, compared against a
--                literal in START WITH, made loaddb's type inference fail with
--                `Cannot coerce _utf8'..' to type unknown data type` on 11.4.5. The
--                10.2 dump of this view carries exactly that -- two ADD QUERY
--                statements, the second one `select NA,NA,NA,NA from (select NA,NA,NA,NA
--                union select NA,NA,NA,NA from ..) [A] (..) start with
--                [A].[PARENTOUCODE]=_utf8'10000000' connect by prior ..`. It imports
--                cleanly on 11.5.0.2498, so this is a regression guard rather than a
--                reproduction: nothing else stops the shape from breaking again.

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

CREATE TABLE lg_tree (
  oucode       VARCHAR(20),
  parentoucode VARCHAR(20),
  orgname      VARCHAR(100),
  ouorder      INTEGER
);

CREATE VCLASS lg_v_tree (
  oucode       VARCHAR(20),
  parentoucode VARCHAR(20),
  orgname      VARCHAR(100),
  ouorder      INTEGER
) AS
  SELECT oucode, parentoucode, orgname, ouorder FROM lg_tree;

-- The second query spec. Its shape is the point: a UNION inside a subquery, the
-- subquery aliased, and START WITH comparing one of its columns to a literal.
ALTER VCLASS [lg_v_tree] ADD QUERY
  SELECT [A].[OUCODE], [A].[PARENTOUCODE], [A].[ORGNAME], [A].[OUORDER]
  FROM ( SELECT '10000000' AS [OUCODE], NULL AS [PARENTOUCODE], 'ROOT' AS [ORGNAME], 0 AS [OUORDER]
         UNION
         SELECT [T].[OUCODE], [T].[PARENTOUCODE], [T].[ORGNAME], [T].[OUORDER] FROM [LG_TREE] [T] ) [A]
  START WITH [A].[PARENTOUCODE] = '10000000'
  CONNECT BY PRIOR [A].[OUCODE] = [A].[PARENTOUCODE]
  ORDER SIBLINGS BY 1;
