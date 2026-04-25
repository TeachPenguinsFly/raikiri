-- Null: the venue did not say, or the row predates journal schema version 3.
ALTER TABLE raikiri.funding_observations
    ADD COLUMN IF NOT EXISTS accrual text;
