-- An instrument id's symbol half is a gateway's load-order index, so one id is
-- one row per gateway generation, valid over [valid_from, valid_to) in unix ns.
-- A row with no generation (0) and no window predates this and matches always.
ALTER TABLE raikiri.instruments
    ADD COLUMN IF NOT EXISTS generation BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS valid_from BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS valid_to   BIGINT;

ALTER TABLE raikiri.instruments
    DROP CONSTRAINT IF EXISTS instruments_venue_market_type_exchange_symbol_key;
ALTER TABLE raikiri.instruments DROP CONSTRAINT IF EXISTS instruments_pkey;
ALTER TABLE raikiri.instruments ADD PRIMARY KEY (instrument_id, generation);

CREATE INDEX IF NOT EXISTS instruments_symbol_window
    ON raikiri.instruments (venue, exchange_symbol, valid_from);
