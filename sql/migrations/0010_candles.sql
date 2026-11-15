-- one row per instrument and minute with prints, written by candled from the gateways' trade rings and from venue
-- history where our own feed has a gap. Prices and volume are decimal e9; a minute without prints has no row
CREATE TABLE IF NOT EXISTS raikiri.candles_1m (
    instrument_id BIGINT   NOT NULL,
    minute        INTEGER  NOT NULL, -- unix minutes
    open          BIGINT   NOT NULL,
    high          BIGINT   NOT NULL,
    low           BIGINT   NOT NULL,
    close         BIGINT   NOT NULL,
    volume        BIGINT   NOT NULL,
    trades        INTEGER  NOT NULL,
    source        SMALLINT NOT NULL, -- 0 our feed, 1 the venue's history
    PRIMARY KEY (instrument_id, minute)
);

-- pruning by age scans minutes, not instruments
CREATE INDEX IF NOT EXISTS candles_1m_minute ON raikiri.candles_1m USING brin (minute);
