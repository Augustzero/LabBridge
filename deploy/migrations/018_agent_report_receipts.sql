CREATE TABLE IF NOT EXISTS agent_report_receipts (
    id BIGSERIAL PRIMARY KEY,
    node_id BIGINT NOT NULL REFERENCES nodes(id),
    task_run_id BIGINT NOT NULL REFERENCES task_runs(id),
    request_type VARCHAR(32) NOT NULL,
    idempotency_key VARCHAR(128) NOT NULL,
    request_fingerprint CHAR(64) NOT NULL,
    response_json JSONB,
    completed_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT agent_report_receipts_request_type_check
        CHECK (request_type IN ('raw_file_manifest', 'task_run_report')),
    CONSTRAINT agent_report_receipts_completion_check
        CHECK ((response_json IS NULL) = (completed_at IS NULL))
);

CREATE UNIQUE INDEX IF NOT EXISTS agent_report_receipts_idempotency_uidx
    ON agent_report_receipts (node_id, request_type, idempotency_key);

CREATE UNIQUE INDEX IF NOT EXISTS agent_report_receipts_task_run_report_uidx
    ON agent_report_receipts (task_run_id)
    WHERE request_type = 'task_run_report';
