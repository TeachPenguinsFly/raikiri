-- everything is keyed so a journal can be replayed from zero

CREATE SCHEMA IF NOT EXISTS raikiri;

CREATE TABLE IF NOT EXISTS raikiri.instruments (
    instrument_id    BIGINT PRIMARY KEY,          -- packed {venue|market|symbol}
    venue            TEXT        NOT NULL,
    market_type      TEXT        NOT NULL,
    local_symbol     TEXT        NOT NULL,
    exchange_symbol  TEXT        NOT NULL,
    tick_size_e9     BIGINT      NOT NULL CHECK (tick_size_e9 > 0),
    lot_size_e9      BIGINT      NOT NULL CHECK (lot_size_e9 > 0),
    min_notional_e9  BIGINT      NOT NULL DEFAULT 0,
    contract_value_e9 BIGINT     NOT NULL DEFAULT 1000000000,
    maker_fee_e9     BIGINT      NOT NULL DEFAULT 0,
    taker_fee_e9     BIGINT      NOT NULL DEFAULT 0,
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (venue, market_type, exchange_symbol)
);

-- current state; the history is in order_events
CREATE TABLE IF NOT EXISTS raikiri.orders (
    client_order_id           BIGINT PRIMARY KEY,  -- packed {strategy|seq}
    exchange_client_order_id  TEXT UNIQUE,         -- ULID sent to the venue
    exchange_order_id         TEXT,
    instrument_id             BIGINT      NOT NULL,
    side                      TEXT        NOT NULL,
    order_type                TEXT        NOT NULL,
    tif                       TEXT        NOT NULL,
    flags                     SMALLINT    NOT NULL DEFAULT 0,
    price_ticks               BIGINT,
    qty_lots                  BIGINT      NOT NULL,
    leaves_lots               BIGINT,
    status                    TEXT        NOT NULL,
    ts_created                BIGINT      NOT NULL, -- UnixNanos
    ts_updated                BIGINT      NOT NULL
);
CREATE INDEX IF NOT EXISTS orders_instrument_idx ON raikiri.orders (instrument_id);
CREATE INDEX IF NOT EXISTS orders_status_idx ON raikiri.orders (status)
    WHERE status IN ('submitted', 'accepted', 'partially_filled',
                     'pending_update', 'pending_cancel', 'triggered');

-- keyed on (journal, journal_seq) so replay is safe
CREATE TABLE IF NOT EXISTS raikiri.order_events (
    journal          TEXT        NOT NULL,
    journal_seq      BIGINT      NOT NULL,
    client_order_id  BIGINT      NOT NULL,
    event_type       TEXT        NOT NULL,
    instrument_id    BIGINT      NOT NULL,
    price_ticks      BIGINT,
    qty_lots         BIGINT,
    leaves_lots      BIGINT,
    fee_e9           BIGINT,
    exec_id          TEXT,
    ts_event         BIGINT      NOT NULL,
    ts_recorded      BIGINT      NOT NULL,
    PRIMARY KEY (journal, journal_seq)
);
CREATE INDEX IF NOT EXISTS order_events_coid_idx ON raikiri.order_events (client_order_id);

-- dedupe on the venue's (venue, exec_id)
CREATE TABLE IF NOT EXISTS raikiri.executions (
    venue            TEXT   NOT NULL,
    exec_id          TEXT   NOT NULL,
    exchange_order_id TEXT,
    client_order_id  BIGINT,
    instrument_id    BIGINT NOT NULL,
    side             TEXT   NOT NULL,
    price_ticks      BIGINT NOT NULL,
    qty_lots         BIGINT NOT NULL,
    fee_e9           BIGINT NOT NULL DEFAULT 0,
    liquidity        TEXT   NOT NULL DEFAULT 'unknown',
    ts_event         BIGINT NOT NULL,
    PRIMARY KEY (venue, exec_id)
);

CREATE TABLE IF NOT EXISTS raikiri.unattributed_fills (
    venue            TEXT   NOT NULL,
    exec_id          TEXT   NOT NULL,
    payload          JSONB  NOT NULL,
    ts_seen          TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (venue, exec_id)
);

CREATE TABLE IF NOT EXISTS raikiri.pnl_increments (
    source_id        TEXT PRIMARY KEY,             -- e.g. 'fill:bybit:12345'
    account_id       TEXT   NOT NULL,
    kind             TEXT   NOT NULL,              -- fee | realized | funding
    amount_e9        BIGINT NOT NULL,
    ts_event         BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS pnl_increments_account_idx
    ON raikiri.pnl_increments (account_id, ts_event);

CREATE TABLE IF NOT EXISTS raikiri.funding_observations (
    instrument_id    BIGINT NOT NULL,
    ts_event         BIGINT NOT NULL,
    rate_e9          BIGINT NOT NULL,
    predicted_e9     BIGINT,
    mark_ticks       BIGINT,
    next_funding_ts  BIGINT,
    interval_s       INT,
    PRIMARY KEY (instrument_id, ts_event)
);

CREATE TABLE IF NOT EXISTS raikiri.connectivity_events (
    journal          TEXT   NOT NULL,
    journal_seq      BIGINT NOT NULL,
    venue            TEXT   NOT NULL,
    event            TEXT   NOT NULL,
    stream_id        INT    NOT NULL DEFAULT 0,
    ts_recorded      BIGINT NOT NULL,
    PRIMARY KEY (journal, journal_seq)
);

CREATE TABLE IF NOT EXISTS raikiri.latency_samples (
    journal          TEXT   NOT NULL,
    journal_seq      BIGINT NOT NULL,
    venue            TEXT   NOT NULL,
    metric           INT    NOT NULL,
    p50_ns           BIGINT NOT NULL,
    p99_ns           BIGINT NOT NULL,
    p999_ns          BIGINT NOT NULL,
    max_ns           BIGINT NOT NULL,
    sample_count     BIGINT NOT NULL,
    ts_recorded      BIGINT NOT NULL,
    PRIMARY KEY (journal, journal_seq)
);

-- advanced in the same transaction as each batch
CREATE TABLE IF NOT EXISTS raikiri.persist_cursors (
    journal          TEXT PRIMARY KEY,             -- e.g. 'gatewayd-bybit'
    last_seq         BIGINT NOT NULL,
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);
