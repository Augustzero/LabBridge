export type Id = string
export type NullableUtc = string | null
export type JsonObject = Record<string, unknown>

export interface Page<T> {
  items: T[]
  next_cursor: string | null
  has_more: boolean
}

export interface PageInput {
  limit?: number
  cursor?: string
}

export type NodeStatus = 'online' | 'offline'
export type TaskRunStatus = 'pending' | 'running' | 'succeeded' | 'failed'

export interface NodeRecord {
  id: Id
  node_code: string
  name: string
  agent_version: string
  stored_status: NodeStatus
  effective_status: NodeStatus
  last_heartbeat_at: NullableUtc
  created_at: NullableUtc
  updated_at: NullableUtc
}

export interface TaskRunRecord {
  id: Id
  task_id: Id
  node_code: string
  status: TaskRunStatus
  started_at: NullableUtc
  finished_at: NullableUtc
  scheduled_for: NullableUtc
  trigger_type: string
  execution_key: string | null
  items_total: number
  items_success: number
  items_failed: number
  error_summary: string | null
  stale: boolean
  stale_after_seconds: number
}

export interface NodeSummary extends NodeRecord {
  enabled_task_count: number
  disabled_task_count: number
  open_alert_count: number
  latest_task_run: Omit<TaskRunRecord, 'stale' | 'stale_after_seconds'> | null
}

export interface TaskRecord {
  id: Id
  node_code: string
  data_source_id: Id
  name: string
  task_type: string
  schedule_expr: string
  parser_type: string
  qc_profile: string
  qc_rule_ids: Id[]
  enabled: boolean
  created_at: NullableUtc
  updated_at: NullableUtc
}

export interface TaskRunSummary extends TaskRunRecord {
  raw_file_count: number
  parsed_record_count: number
  qc_result_count: number
  alert_count: number
}

export interface RawFileRecord {
  id: Id
  task_run_id: Id
  node_code: string
  original_name: string
  file_hash: string
  storage_path: string
  size_bytes: number
  source_mtime: NullableUtc
  ingest_status: string
  created_at: NullableUtc
}

export interface ParsedRecord {
  id: Id
  raw_file_id: Id
  task_run_id: Id
  station_code: string
  device_code: string
  record_time: string
  payload: JsonObject
  parse_status: string
  created_at: NullableUtc
}

export interface QcResultRecord {
  id: Id
  task_run_id: Id
  parsed_record_id: Id
  qc_rule_id: Id
  level: string
  result: string
  message: string
  created_at: NullableUtc
}

export interface AlertRecord {
  id: Id
  node_code: string
  task_run_id: Id
  alert_type: string
  severity: string
  message: string
  status: string
  created_at: NullableUtc
}
