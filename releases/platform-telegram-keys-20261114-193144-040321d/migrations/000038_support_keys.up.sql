ALTER TABLE users ADD COLUMN supporter_since timestamptz;

CREATE TABLE support_keys (
    id uuid PRIMARY KEY,
    code_hash text NOT NULL UNIQUE,
    code_suffix text NOT NULL,
    label text NOT NULL DEFAULT '',
    created_by uuid REFERENCES admin_users(id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    redeemed_by uuid REFERENCES users(id) ON DELETE SET NULL,
    redeemed_at timestamptz,
    revoked_at timestamptz,
    CHECK (redeemed_at IS NULL OR revoked_at IS NULL),
    CHECK (redeemed_by IS NULL OR redeemed_at IS NOT NULL)
);
CREATE INDEX support_keys_created ON support_keys (created_at DESC, id);
