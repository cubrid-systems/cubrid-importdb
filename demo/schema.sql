-- demo/schema.sql -- the schema the demo unloads and then reloads two ways.
--
-- Shaped so the interesting parts of cubrid-importdb have something to chew on:
--
--   * FK edges form three dependency levels
--         L0  region, product, audit_log   (no FK -- more than one member, so
--                                           --degree has something to fan out over)
--         L1  customer                     (FK -> region)
--         L2  orders                       (FK -> customer, FK -> product)
--   * a PRIMARY KEY on every table
--   * a UNIQUE constraint separate from the PK  (region, product, customer)
--   * a plain secondary index                   (ix_customer_city, ix_orders_qty)
--   * an AUTO_INCREMENT column                  (audit_log.entry_id -- carries a
--                                                serial through the round-trip)
--
-- Row counts stay small on purpose: this is a demo, not a benchmark.

CREATE TABLE region (
  region_id   INTEGER      NOT NULL,
  region_code VARCHAR(8)   NOT NULL,
  region_name VARCHAR(40),
  CONSTRAINT pk_region      PRIMARY KEY (region_id),
  CONSTRAINT uk_region_code UNIQUE (region_code)
);

CREATE TABLE product (
  product_id INTEGER     NOT NULL,
  sku        VARCHAR(16) NOT NULL,
  price      INTEGER,
  CONSTRAINT pk_product      PRIMARY KEY (product_id),
  CONSTRAINT uk_product_sku  UNIQUE (sku)
);

CREATE TABLE audit_log (
  entry_id INTEGER AUTO_INCREMENT NOT NULL,
  note     VARCHAR(60),
  CONSTRAINT pk_audit_log PRIMARY KEY (entry_id)
);

CREATE TABLE customer (
  customer_id INTEGER     NOT NULL,
  region_id   INTEGER     NOT NULL,
  email       VARCHAR(48) NOT NULL,
  city        VARCHAR(24),
  CONSTRAINT pk_customer         PRIMARY KEY (customer_id),
  CONSTRAINT uk_customer_email   UNIQUE (email),
  CONSTRAINT fk_customer_region  FOREIGN KEY (region_id) REFERENCES region(region_id)
);

CREATE INDEX ix_customer_city ON customer(city);

CREATE TABLE orders (
  order_id    INTEGER NOT NULL,
  customer_id INTEGER NOT NULL,
  product_id  INTEGER NOT NULL,
  qty         INTEGER,
  CONSTRAINT pk_orders           PRIMARY KEY (order_id),
  CONSTRAINT fk_orders_customer  FOREIGN KEY (customer_id) REFERENCES customer(customer_id),
  CONSTRAINT fk_orders_product   FOREIGN KEY (product_id)  REFERENCES product(product_id)
);

CREATE INDEX ix_orders_qty ON orders(qty);
