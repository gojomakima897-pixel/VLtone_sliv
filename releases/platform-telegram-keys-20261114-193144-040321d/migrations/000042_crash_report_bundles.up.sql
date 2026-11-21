-- Standalone desktop reports are sent only after per-report consent.
ALTER TABLE crash_reports ALTER COLUMN user_id DROP NOT NULL;
ALTER TABLE crash_reports ALTER COLUMN device_id DROP NOT NULL;
