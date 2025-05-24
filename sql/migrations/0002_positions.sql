-- Positions and the money ledger behind them.
--
-- pnl_increments is the truth for PnL. positions is a projection: every row in
-- it is reconstructable by replaying the ledger, and nothing should ever read
-- it as authoritative.

-- The table shipped in 0001 with an account_id and no writer. The key that
-- actually addresses a position is (strategy, instrument), and the instrument
-- has to be named by something that survives a restart: the symbol half of a
-- packed instrument_id is a catalogue load-order index, so a delisting shifts
-- it and old rows silently point somewhere else.
ALTER TABLE raikiri.pnl_increments
    ADD COLUMN IF NOT EXISTS strategy_id     SMALLINT NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS venue           TEXT     NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS exchange_symbol TEXT     NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS instrument_id   BIGINT,
    ADD COLUMN IF NOT EXISTS client_order_id BIGINT,
    ADD COLUMN IF NOT EXISTS settle_side     TEXT     NOT NULL DEFAULT 'quote'
        CHECK (settle_side IN ('base', 'quote'));

DROP INDEX IF EXISTS raikiri.pnl_increments_account_idx;
CREATE INDEX IF NOT EXISTS pnl_increments_book_idx
    ON raikiri.pnl_increments (strategy_id, venue, exchange_symbol, ts_event);

-- Which PnL formula the instrument obeys. Without it a bitmex XBTUSD row and a
-- binance BTCUSDT row are indistinguishable, and one of the two prices wrong.
ALTER TABLE raikiri.instruments
    ADD COLUMN IF NOT EXISTS contract_kind TEXT NOT NULL DEFAULT 'unknown'
        CHECK (contract_kind IN ('unknown', 'linear', 'inverse'));

-- REBUILDABLE CACHE, not truth. cost_e9 is what the OPEN quantity cost, in the
-- instrument's settlement currency; there is no average entry price column
-- because an average drifts on partial closes and invites importing the
-- venue's, and the venue is authoritative on whether a position exists and
-- never on what it cost.
CREATE TABLE IF NOT EXISTS raikiri.positions (
    strategy_id     SMALLINT NOT NULL,
    venue           TEXT     NOT NULL,
    exchange_symbol TEXT     NOT NULL,
    instrument_id   BIGINT,
    net_qty_lots    BIGINT   NOT NULL,
    cost_e9         BIGINT   NOT NULL,
    realized_e9     BIGINT   NOT NULL DEFAULT 0,
    fees_e9         BIGINT   NOT NULL DEFAULT 0,
    settle_side     TEXT     NOT NULL CHECK (settle_side IN ('base', 'quote')),
    fill_count      INTEGER  NOT NULL DEFAULT 0,
    ts_opened       BIGINT   NOT NULL,
    ts_updated      BIGINT   NOT NULL,
    PRIMARY KEY (strategy_id, venue, exchange_symbol)
);

-- A reconciliation break is a fact worth keeping, not a log line: it is the
-- record that the venue and this process disagreed, and by how much.
CREATE TABLE IF NOT EXISTS raikiri.recon_breaks (
    venue           TEXT   NOT NULL,
    exchange_symbol TEXT   NOT NULL,
    ts_seen         BIGINT NOT NULL,
    local_qty_lots  BIGINT NOT NULL,
    venue_qty_lots  BIGINT NOT NULL,
    detail          TEXT   NOT NULL,
    PRIMARY KEY (venue, exchange_symbol, ts_seen)
);
