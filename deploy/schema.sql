CREATE TABLE IF NOT EXISTS nodes (
    id BIGSERIAL PRIMARY KEY,
    node_code VARCHAR(64) NOT NULL UNIQUE,
    name VARCHAR(128) NOT NULL,
    location VARCHAR(255),
    status VARCHAR(32) NOT NULL DEFAULT 'offline',
    agent_version VARCHAR(64),
    last_heartbeat_at TIMESTAMPTZ,
    last_seen_ip VARCHAR(64),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS data_sources (
    id BIGSERIAL PRIMARY KEY,
    node_id BIGINT NOT NULL REFERENCES nodes(id),
    source_type VARCHAR(32) NOT NULL,
    name VARCHAR(128) NOT NULL,
    config_json JSONB NOT NULL,
    enabled BOOLEAN NOT NULL DEFAULT true,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS tasks (
    id BIGSERIAL PRIMARY KEY,
    node_id BIGINT NOT NULL REFERENCES nodes(id),
    data_source_id BIGINT NOT NULL REFERENCES data_sources(id),
    name VARCHAR(128) NOT NULL,
    task_type VARCHAR(32) NOT NULL,
    schedule_expr VARCHAR(128) NOT NULL,
    parser_type VARCHAR(64) NOT NULL,
    qc_profile VARCHAR(64),
    enabled BOOLEAN NOT NULL DEFAULT true,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS task_runs (
    id BIGSERIAL PRIMARY KEY,
    task_id BIGINT NOT NULL REFERENCES tasks(id),
    node_id BIGINT NOT NULL REFERENCES nodes(id),
    status VARCHAR(32) NOT NULL,
    started_at TIMESTAMPTZ,
    finished_at TIMESTAMPTZ,
    duration_ms BIGINT,
    items_total INTEGER NOT NULL DEFAULT 0,
    items_success INTEGER NOT NULL DEFAULT 0,
    items_failed INTEGER NOT NULL DEFAULT 0,
    error_summary TEXT,
    trigger_type VARCHAR(32) NOT NULL DEFAULT 'scheduled',
    execution_key VARCHAR(128),
    scheduled_for TIMESTAMPTZ
);

CREATE UNIQUE INDEX IF NOT EXISTS task_runs_node_execution_key_uidx
    ON task_runs (node_id, execution_key)
    WHERE execution_key IS NOT NULL;

CREATE TABLE IF NOT EXISTS raw_files (
    id BIGSERIAL PRIMARY KEY,
    task_run_id BIGINT NOT NULL REFERENCES task_runs(id),
    node_id BIGINT NOT NULL REFERENCES nodes(id),
    original_name VARCHAR(255) NOT NULL,
    file_hash VARCHAR(128),
    storage_path TEXT NOT NULL,
    size_bytes BIGINT NOT NULL DEFAULT 0,
    source_mtime TIMESTAMPTZ,
    ingest_status VARCHAR(32) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS parsed_records (
    id BIGSERIAL PRIMARY KEY,
    raw_file_id BIGINT REFERENCES raw_files(id),
    task_run_id BIGINT NOT NULL REFERENCES task_runs(id),
    station_code VARCHAR(64),
    device_code VARCHAR(64),
    record_time TIMESTAMPTZ,
    payload_json JSONB NOT NULL,
    parse_status VARCHAR(32) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS qc_rules (
    id BIGSERIAL PRIMARY KEY,
    name VARCHAR(128) NOT NULL,
    rule_type VARCHAR(64) NOT NULL,
    rule_config_json JSONB NOT NULL,
    enabled BOOLEAN NOT NULL DEFAULT true,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS task_qc_rules (
    task_id BIGINT NOT NULL REFERENCES tasks(id),
    qc_rule_id BIGINT NOT NULL REFERENCES qc_rules(id),
    sort_order INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (task_id, qc_rule_id)
);

CREATE INDEX IF NOT EXISTS task_qc_rules_task_sort_idx
    ON task_qc_rules (task_id, sort_order, qc_rule_id);

CREATE TABLE IF NOT EXISTS qc_results (
    id BIGSERIAL PRIMARY KEY,
    parsed_record_id BIGINT NOT NULL REFERENCES parsed_records(id),
    qc_rule_id BIGINT NOT NULL REFERENCES qc_rules(id),
    level VARCHAR(32) NOT NULL,
    result VARCHAR(32) NOT NULL,
    message TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS alerts (
    id BIGSERIAL PRIMARY KEY,
    node_id BIGINT REFERENCES nodes(id),
    task_run_id BIGINT REFERENCES task_runs(id),
    alert_type VARCHAR(64) NOT NULL,
    severity VARCHAR(32) NOT NULL,
    message TEXT NOT NULL,
    status VARCHAR(32) NOT NULL DEFAULT 'open',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

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
