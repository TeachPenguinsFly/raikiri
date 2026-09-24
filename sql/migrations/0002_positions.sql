-- positions is a projection of pnl_increments and can be rebuilt from it

-- the symbol half of instrument_id is a load-order index; key on something that survives a restart
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

-- which pnl formula the instrument uses
ALTER TABLE raikiri.instruments
    ADD COLUMN IF NOT EXISTS contract_kind TEXT NOT NULL DEFAULT 'unknown'
        CHECK (contract_kind IN ('unknown', 'linear', 'inverse'));

-- rebuildable cache; cost_e9 is the open quantity's cost in settlement currency
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

CREATE TABLE IF NOT EXISTS raikiri.recon_breaks (
    venue           TEXT   NOT NULL,
    exchange_symbol TEXT   NOT NULL,
    ts_seen         BIGINT NOT NULL,
    local_qty_lots  BIGINT NOT NULL,
    venue_qty_lots  BIGINT NOT NULL,
    detail          TEXT   NOT NULL,
    PRIMARY KEY (venue, exchange_symbol, ts_seen)
);
