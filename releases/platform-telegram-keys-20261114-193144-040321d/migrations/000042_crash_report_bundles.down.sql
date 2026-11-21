-- Refuse rollback while it would discard standalone reports.
DO $$ BEGIN
    IF EXISTS (SELECT 1 FROM crash_reports WHERE user_id IS NULL OR device_id IS NULL) THEN
        RAISE EXCEPTION 'Standalone crash reports must be retained or explicitly removed before rollback';
    END IF;
END $$;
ALTER TABLE crash_reports ALTER COLUMN user_id SET NOT NULL;
ALTER TABLE crash_reports ALTER COLUMN device_id SET NOT NULL;
