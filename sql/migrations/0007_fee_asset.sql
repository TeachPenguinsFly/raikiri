-- the currency a fee was charged in, null where the venue stated none
ALTER TABLE raikiri.order_events ADD COLUMN IF NOT EXISTS fee_asset TEXT;
ALTER TABLE raikiri.executions ADD COLUMN IF NOT EXISTS fee_asset TEXT;
