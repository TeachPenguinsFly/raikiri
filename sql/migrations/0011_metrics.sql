-- one row per instrument and minute, sampled by candled from the gateways' funding and ticker cells at the start of the
-- minute. Values are decimal e9, the open interest in the base asset; a column is null where the venue publishes none,
-- and an instrument with none of the four has no row
CREATE TABLE IF NOT EXISTS raikiri.metrics_1m (
    instrument_id BIGINT  NOT NULL,
    minute        INTEGER NOT NULL, -- unix minutes
    funding_rate  BIGINT,           -- per interval, not an APR
    mark_price    BIGINT,
    index_price   BIGINT,
    open_interest BIGINT,
    PRIMARY KEY (instrument_id, minute)
);

-- pruning by age scans minutes, not instruments
CREATE INDEX IF NOT EXISTS metrics_1m_minute ON raikiri.metrics_1m USING brin (minute);
