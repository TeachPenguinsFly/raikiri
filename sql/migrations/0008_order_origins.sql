-- whose an order is and why it exists; the book's id is client_order_id >> 48, so an order older than this table
-- simply has no row here and still has a book
CREATE TABLE IF NOT EXISTS raikiri.order_origins (
    client_order_id  BIGINT PRIMARY KEY,
    book_id          INTEGER NOT NULL,
    book_kind        TEXT    NOT NULL,
    parent_id        BIGINT,
    external_ref     TEXT,
    ts_created       BIGINT  NOT NULL
);
CREATE INDEX IF NOT EXISTS order_origins_parent_idx ON raikiri.order_origins (parent_id) WHERE parent_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS order_origins_ref_idx ON raikiri.order_origins (external_ref) WHERE external_ref IS NOT NULL;

CREATE INDEX IF NOT EXISTS orders_book_idx ON raikiri.orders ((client_order_id >> 48), ts_created DESC);
CREATE INDEX IF NOT EXISTS executions_book_idx ON raikiri.executions ((client_order_id >> 48), ts_event DESC);
