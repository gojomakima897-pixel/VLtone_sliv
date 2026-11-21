ALTER TABLE support_keys ADD COLUMN device_limit integer NOT NULL DEFAULT 1
    CHECK (device_limit BETWEEN 1 AND 100);

CREATE TABLE support_key_activations (
    support_key_id uuid NOT NULL REFERENCES support_keys(id) ON DELETE CASCADE,
    installation_id uuid NOT NULL CHECK (installation_id <> '00000000-0000-0000-0000-000000000000'),
    user_id uuid REFERENCES users(id) ON DELETE SET NULL,
    activated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (support_key_id, installation_id)
);
CREATE INDEX support_key_activations_user_installation
    ON support_key_activations (user_id, installation_id) WHERE user_id IS NOT NULL;

-- Keep existing installation bindings and the timestamps of their signed receipts.
INSERT INTO support_key_activations (support_key_id, installation_id, activated_at)
SELECT id, redeemed_installation_id, redeemed_at FROM support_keys
WHERE redeemed_installation_id IS NOT NULL;
