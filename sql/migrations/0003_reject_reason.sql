-- Why a venue refused an order, beside the fact that it did.
--
-- `/v1/orders` could say "rejected" and nothing else: the cause reached a log
-- line on whichever machine ran the gateway and stopped there. A refusal whose
-- reason only exists in a log is a refusal nobody investigates.
--
-- NULLABLE ON PURPOSE. Null means "not recorded" — an event that is not a
-- rejection, or one journalled before schema version 2 carried these — which is
-- a different fact from the venue having given no reason, and a query can tell
-- them apart.
ALTER TABLE raikiri.order_events
    ADD COLUMN IF NOT EXISTS reject_class text,
    ADD COLUMN IF NOT EXISTS reject_code  integer;

ALTER TABLE raikiri.orders
    ADD COLUMN IF NOT EXISTS reject_class text,
    ADD COLUMN IF NOT EXISTS reject_code  integer;

-- The question this is for: what is this venue refusing, and is it the same
-- thing every time.
CREATE INDEX IF NOT EXISTS order_events_reject_idx
    ON raikiri.order_events (reject_class, reject_code)
    WHERE reject_class IS NOT NULL;
