CREATE TABLE IF NOT EXISTS task_qc_rules (
    task_id BIGINT NOT NULL REFERENCES tasks(id),
    qc_rule_id BIGINT NOT NULL REFERENCES qc_rules(id),
    sort_order INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (task_id, qc_rule_id)
);

CREATE INDEX IF NOT EXISTS task_qc_rules_task_sort_idx
    ON task_qc_rules (task_id, sort_order, qc_rule_id);
