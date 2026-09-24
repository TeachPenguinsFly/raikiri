-- null means not recorded, which differs from the venue giving no reason
ALTER TABLE raikiri.order_events
    ADD COLUMN IF NOT EXISTS reject_class text,
    ADD COLUMN IF NOT EXISTS reject_code  integer;

ALTER TABLE raikiri.orders
    ADD COLUMN IF NOT EXISTS reject_class text,
    ADD COLUMN IF NOT EXISTS reject_code  integer;

CREATE INDEX IF NOT EXISTS order_events_reject_idx
    ON raikiri.order_events (reject_class, reject_code)
    WHERE reject_class IS NOT NULL;
