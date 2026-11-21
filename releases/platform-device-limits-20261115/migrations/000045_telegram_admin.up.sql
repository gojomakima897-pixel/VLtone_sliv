CREATE TABLE telegram_admin_bindings (
    chat_id bigint PRIMARY KEY CHECK (chat_id > 0),
    telegram_user_id bigint NOT NULL CHECK (telegram_user_id = chat_id),
    bot_key text NOT NULL,
    session_id uuid NOT NULL UNIQUE REFERENCES admin_sessions(id) ON DELETE CASCADE,
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE telegram_admin_notifications (
    id bigserial PRIMARY KEY,
    chat_id bigint NOT NULL REFERENCES telegram_admin_bindings(chat_id) ON DELETE CASCADE,
    kind text NOT NULL CHECK (kind IN ('crash', 'bug')),
    report_id uuid NOT NULL,
    attempts integer NOT NULL DEFAULT 0,
    next_attempt_at timestamptz NOT NULL DEFAULT now(),
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (chat_id, kind, report_id)
);
CREATE INDEX telegram_admin_notifications_due_idx ON telegram_admin_notifications(next_attempt_at, id);

CREATE TABLE telegram_bot_offsets (
    name text PRIMARY KEY,
    update_id bigint NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now()
);
