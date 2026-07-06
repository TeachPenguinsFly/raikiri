-- What the contract tracks, so an equity perp on one venue and its namesake
-- memecoin on another never share a row.
ALTER TABLE raikiri.instruments
    ADD COLUMN IF NOT EXISTS asset_class TEXT NOT NULL DEFAULT 'unknown'
        CHECK (asset_class IN ('unknown', 'crypto', 'equity', 'index', 'commodity', 'fx')),
    ADD COLUMN IF NOT EXISTS underlying TEXT NOT NULL DEFAULT '';
