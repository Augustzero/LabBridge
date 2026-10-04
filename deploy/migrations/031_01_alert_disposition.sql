-- 031-01 告警处置：记录首次确认与关闭时间，历史数据保持 NULL。
ALTER TABLE alerts
    ADD COLUMN IF NOT EXISTS acknowledged_at TIMESTAMPTZ,
    ADD COLUMN IF NOT EXISTS closed_at TIMESTAMPTZ;
