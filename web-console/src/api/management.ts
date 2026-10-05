import { request } from './http'
import type {
  Alert,
  DataSource,
  ManualTaskRunAcceptance,
  Node,
  NodeSummary,
  Page,
  ParsedRecord,
  QcResult,
  QcRule,
  RawFile,
  Task,
  TaskRunDetail,
  TaskRunWithStale,
} from './types'

export interface PageQuery {
  limit?: number
  cursor?: string
}

export interface ListNodesQuery extends PageQuery {
  status?: 'online' | 'offline'
}

export interface ListDataSourcesQuery extends PageQuery {
  nodeCode: string
  enabled?: boolean
}

export interface ListQcRulesQuery extends PageQuery {
  enabled?: boolean
}

export interface ListTasksQuery extends PageQuery {
  nodeCode: string
  enabled?: boolean
}

export interface ListTaskRunsQuery extends PageQuery {
  nodeCode: string
  taskId?: string
  status?: string
}

export interface ListRawFilesQuery extends PageQuery {
  taskRunId: string
}

export interface ListParsedRecordsQuery extends PageQuery {
  taskRunId: string
}

export interface ListQcResultsQuery extends PageQuery {
  taskRunId: string
  result?: 'passed' | 'failed'
}

export interface ListAlertsQuery extends PageQuery {
  nodeCode: string
  taskRunId?: string
  status?: string
  severity?: string
}

type QueryValue = string | number | boolean | undefined

// 服务端对 query 参数做严格白名单校验，未列出的参数直接 400；
// undefined 字段必须在此处剔除，绝不发送空参数。
function buildQuery(values: Record<string, QueryValue>): Record<string, string> {
  const query: Record<string, string> = {}
  for (const [key, value] of Object.entries(values)) {
    if (value !== undefined) {
      query[key] = String(value)
    }
  }
  return query
}

export function listNodes(
  query: ListNodesQuery,
  signal?: AbortSignal,
): Promise<Page<Node>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/nodes',
      params: buildQuery({
        status: query.status,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

export function findNode(
  nodeCode: string,
  signal?: AbortSignal,
): Promise<NodeSummary> {
  return request(
    { method: 'get', url: `/api/v1/nodes/${encodeURIComponent(nodeCode)}` },
    signal,
  )
}

export function listDataSources(
  query: ListDataSourcesQuery,
  signal?: AbortSignal,
): Promise<Page<DataSource>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/data-sources',
      params: buildQuery({
        node_code: query.nodeCode,
        enabled: query.enabled,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

export function listQcRules(
  query: ListQcRulesQuery,
  signal?: AbortSignal,
): Promise<Page<QcRule>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/qc-rules',
      params: buildQuery({
        enabled: query.enabled,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

export function listTasks(
  query: ListTasksQuery,
  signal?: AbortSignal,
): Promise<Page<Task>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/tasks',
      params: buildQuery({
        node_code: query.nodeCode,
        enabled: query.enabled,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

export function listTaskRuns(
  query: ListTaskRunsQuery,
  signal?: AbortSignal,
): Promise<Page<TaskRunWithStale>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/task-runs',
      params: buildQuery({
        node_code: query.nodeCode,
        task_id: query.taskId,
        status: query.status,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

export function findTaskRun(
  taskRunId: string,
  nodeCode: string,
  signal?: AbortSignal,
): Promise<TaskRunDetail> {
  return request(
    {
      method: 'get',
      url: `/api/v1/task-runs/${encodeURIComponent(taskRunId)}`,
      params: buildQuery({ node_code: nodeCode }),
    },
    signal,
  )
}

export function listRawFiles(
  query: ListRawFilesQuery,
  signal?: AbortSignal,
): Promise<Page<RawFile>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/raw-files',
      params: buildQuery({
        task_run_id: query.taskRunId,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

export function listParsedRecords(
  query: ListParsedRecordsQuery,
  signal?: AbortSignal,
): Promise<Page<ParsedRecord>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/parsed-records',
      params: buildQuery({
        task_run_id: query.taskRunId,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

export function listQcResults(
  query: ListQcResultsQuery,
  signal?: AbortSignal,
): Promise<Page<QcResult>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/qc-results',
      params: buildQuery({
        task_run_id: query.taskRunId,
        result: query.result,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

export function listAlerts(
  query: ListAlertsQuery,
  signal?: AbortSignal,
): Promise<Page<Alert>> {
  return request(
    {
      method: 'get',
      url: '/api/v1/alerts',
      params: buildQuery({
        node_code: query.nodeCode,
        task_run_id: query.taskRunId,
        status: query.status,
        severity: query.severity,
        limit: query.limit,
        cursor: query.cursor,
      }),
    },
    signal,
  )
}

// 写操作区：请求期间由调用方禁用入口，失败统一抛 ApiError 由 ErrorBanner 呈现。

// 任务启停：服务端仅接受 {"enabled": bool} 单字段 body。
export function setTaskEnabled(
  taskId: string,
  enabled: boolean,
  signal?: AbortSignal,
): Promise<Task> {
  return request(
    {
      method: 'patch',
      url: `/api/v1/tasks/${encodeURIComponent(taskId)}`,
      data: { enabled },
    },
    signal,
  )
}

// 人工提交类请求带独立超时：响应丢失时按超时处理，前端保留幂等键等确认，
// 不会因为列表页共用 httpClient 无超时的配置而无限挂起。
const MANUAL_SUBMIT_TIMEOUT_MS = 15000

// 对已启用任务提交一次目录扫描（不等 Cron 槽位），202 只表示请求已保存。
export function triggerTask(
  taskId: string,
  idempotencyKey: string,
  signal?: AbortSignal,
): Promise<ManualTaskRunAcceptance> {
  return request(
    {
      method: 'post',
      url: `/api/v1/tasks/${encodeURIComponent(taskId)}/trigger`,
      data: { idempotency_key: idempotencyKey },
      timeout: MANUAL_SUBMIT_TIMEOUT_MS,
    },
    signal,
  )
}

// 对终态 failed 且有失败清单的运行创建重试运行，输入范围由服务端固定。
export function retryTaskRun(
  taskRunId: string,
  idempotencyKey: string,
  signal?: AbortSignal,
): Promise<ManualTaskRunAcceptance> {
  return request(
    {
      method: 'post',
      url: `/api/v1/task-runs/${encodeURIComponent(taskRunId)}/retry`,
      data: { idempotency_key: idempotencyKey },
      timeout: MANUAL_SUBMIT_TIMEOUT_MS,
    },
    signal,
  )
}

// 告警处置：服务端只接受空 JSON 对象，成功 200 返回最新告警。
export function acknowledgeAlert(
  alertId: string,
  signal?: AbortSignal,
): Promise<Alert> {
  return request(
    {
      method: 'post',
      url: `/api/v1/alerts/${encodeURIComponent(alertId)}/acknowledge`,
      data: {},
    },
    signal,
  )
}

export function closeAlert(
  alertId: string,
  signal?: AbortSignal,
): Promise<Alert> {
  return request(
    {
      method: 'post',
      url: `/api/v1/alerts/${encodeURIComponent(alertId)}/close`,
      data: {},
    },
    signal,
  )
}
