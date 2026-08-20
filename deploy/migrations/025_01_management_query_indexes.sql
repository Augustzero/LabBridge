CREATE INDEX IF NOT EXISTS data_sources_node_management_idx
    ON data_sources (node_id, id DESC);

CREATE INDEX IF NOT EXISTS tasks_node_management_idx
    ON tasks (node_id, id DESC);

CREATE INDEX IF NOT EXISTS task_runs_node_management_idx
    ON task_runs (node_id, id DESC);

CREATE INDEX IF NOT EXISTS task_runs_task_management_idx
    ON task_runs (task_id, id DESC);

CREATE INDEX IF NOT EXISTS raw_files_task_run_management_idx
    ON raw_files (task_run_id, id DESC);

CREATE INDEX IF NOT EXISTS parsed_records_task_run_management_idx
    ON parsed_records (task_run_id, id DESC);

CREATE INDEX IF NOT EXISTS qc_results_parsed_record_management_idx
    ON qc_results (parsed_record_id, id DESC);

CREATE INDEX IF NOT EXISTS alerts_node_management_idx
    ON alerts (node_id, id DESC);

CREATE INDEX IF NOT EXISTS alerts_task_run_management_idx
    ON alerts (task_run_id, id DESC);
