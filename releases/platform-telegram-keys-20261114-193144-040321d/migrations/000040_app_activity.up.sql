-- Anonymous aggregate counts only. Run nonces exist solely in API memory.
CREATE TABLE app_launch_hours (
    hour timestamptz PRIMARY KEY,
    launches bigint NOT NULL CHECK (launches > 0),
    last_launch_at timestamptz NOT NULL
);
