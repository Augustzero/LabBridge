-- 031-02 任务人工执行与失败文件重试：
-- task_runs 记录人工执行意图与固定重试输入，历史数据保持 NULL。
ALTER TABLE task_runs
    ADD COLUMN IF NOT EXISTS requested_at TIMESTAMPTZ,
    ADD COLUMN IF NOT EXISTS retry_of_run_id BIGINT REFERENCES task_runs(id),
    ADD COLUMN IF NOT EXISTS failed_files JSONB,
    ADD COLUMN IF NOT EXISTS retry_files JSONB;

-- 同一任务最多保留一个尚未开始的人工请求，由部分唯一索引裁决并发。
CREATE UNIQUE INDEX IF NOT EXISTS task_runs_task_manual_pending_uidx
    ON task_runs (task_id)
    WHERE status = 'pending' AND trigger_type IN ('manual', 'retry');
