ALTER TABLE task_runs
    ADD COLUMN IF NOT EXISTS execution_key VARCHAR(128),
    ADD COLUMN IF NOT EXISTS scheduled_for TIMESTAMPTZ;

CREATE UNIQUE INDEX IF NOT EXISTS task_runs_node_execution_key_uidx
    ON task_runs (node_id, execution_key)
    WHERE execution_key IS NOT NULL;
