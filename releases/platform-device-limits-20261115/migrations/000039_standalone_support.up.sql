ALTER TABLE support_keys ADD COLUMN redeemed_installation_id uuid;
ALTER TABLE support_keys ADD CONSTRAINT support_keys_installation_redemption
    CHECK (redeemed_installation_id IS NULL OR (redeemed_at IS NOT NULL AND redeemed_by IS NULL));
