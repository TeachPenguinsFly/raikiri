-- moves with every upsert, so a reader can tell two answers about one order apart
ALTER TABLE raikiri.orders ADD COLUMN IF NOT EXISTS version BIGINT NOT NULL DEFAULT 0;
