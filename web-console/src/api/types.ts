// 管理 API DTO 类型，逐一对应 server/src/http/management_http_controller.cpp 的 JSON 映射。
// 时间字段服务端以 null 表示缺失（如未上报心跳的节点），统一建模为 string | null。

export type NodeStatus = 'online' | 'offline'

export type TaskRunStatus = 'pending' | 'running' | 'succeeded' | 'failed'

export type SourceType = 'local_directory' | 'ftp' | 'oracle'

export interface Node {
  id: string
  node_code: string
  name: string
  agent_version: string
  stored_status: NodeStatus
  effective_status: NodeStatus
  last_heartbeat_at: string | null
  created_at: string | null
  updated_at: string | null
}

export interface NodeSummary extends Node {
  enabled_task_count: number
  disabled_task_count: number
  open_alert_count: number
  latest_task_run: TaskRun | null
}

export interface DataSource {
  id: string
  node_code: string
  source_type: SourceType
  name: string
  config: Record<string, unknown>
  enabled: boolean
  created_at: string | null
  updated_at: string | null
}

export interface QcRule {
  id: string
  name: string
  rule_type: string
  config: Record<string, unknown>
  enabled: boolean
  created_at: string | null
}

export interface Task {
  id: string
  node_code: string
  data_source_id: string
  name: string
  task_type: string
  schedule_expr: string
  parser_type: string
  qc_profile: string
  enabled: boolean
  qc_rule_ids: string[]
  created_at: string | null
  updated_at: string | null
}

export interface TaskRun {
  id: string
  task_id: string
  node_code: string
  status: TaskRunStatus
  started_at: string | null
  finished_at: string | null
  scheduled_for: string | null
  trigger_type: string
  execution_key: string | null
  items_total: number
  items_success: number
  items_failed: number
  error_summary: string | null
}

/** 运行列表项：运行结构 + stale 呈现字段 */
export interface TaskRunWithStale extends TaskRun {
  stale: boolean
  stale_after_seconds: number
}

/** 运行详情：额外携带四类证据计数 */
export interface TaskRunDetail extends TaskRunWithStale {
  raw_file_count: number
  parsed_record_count: number
  qc_result_count: number
  alert_count: number
}

export interface RawFile {
  id: string
  task_run_id: string
  node_code: string
  original_name: string
  file_hash: string
  storage_path: string
  size_bytes: number
  source_mtime: string | null
  ingest_status: string
  created_at: string | null
}

export interface ParsedRecord {
  id: string
  raw_file_id: string
  task_run_id: string
  station_code: string
  device_code: string
  record_time: string
  payload: Record<string, unknown>
  parse_status: string
  created_at: string | null
}

export interface QcResult {
  id: string
  task_run_id: string
  parsed_record_id: string
  qc_rule_id: string
  level: string
  result: 'passed' | 'failed'
  message: string | null
  created_at: string | null
}

export interface Alert {
  id: string
  node_code: string
  task_run_id: string | null
  alert_type: string
  severity: string
  message: string
  status: string
  created_at: string | null
}

/** keyset 分页包装：next_cursor 为 null 表示没有更多页 */
export interface Page<T> {
  items: T[]
  next_cursor: string | null
  has_more: boolean
}
