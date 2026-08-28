import { apiClient } from './client'
import type {
  AlertRecord,
  NodeRecord,
  NodeStatus,
  NodeSummary,
  Page,
  PageInput,
  ParsedRecord,
  QcResultRecord,
  RawFileRecord,
  TaskRecord,
  TaskRunRecord,
  TaskRunStatus,
  TaskRunSummary,
} from './types'

type RequestOptions = { signal?: AbortSignal }

export interface NodeListInput extends PageInput {
  status?: NodeStatus
}

export interface TaskListInput extends PageInput {
  node_code: string
  enabled?: boolean
}

export interface TaskRunListInput extends PageInput {
  node_code: string
  task_id?: string
  status?: TaskRunStatus
}

export interface EvidenceListInput extends PageInput {
  task_run_id: string
}

function get<T>(path: string, params: object, options?: RequestOptions) {
  return apiClient.get<T>(path, { params, signal: options?.signal })
}

export const managementApi = {
  listNodes: (input: NodeListInput = {}, options?: RequestOptions) =>
    get<Page<NodeRecord>>('/nodes', input, options),
  getNode: (nodeCode: string, options?: RequestOptions) =>
    get<NodeSummary>(`/nodes/${encodeURIComponent(nodeCode)}`, {}, options),
  listTasks: (input: TaskListInput, options?: RequestOptions) =>
    get<Page<TaskRecord>>('/tasks', input, options),
  listTaskRuns: (input: TaskRunListInput, options?: RequestOptions) =>
    get<Page<TaskRunRecord>>('/task-runs', input, options),
  getTaskRun: (runId: string, nodeCode: string, options?: RequestOptions) =>
    get<TaskRunSummary>(
      `/task-runs/${encodeURIComponent(runId)}`,
      { node_code: nodeCode },
      options,
    ),
  listRawFiles: (input: EvidenceListInput, options?: RequestOptions) =>
    get<Page<RawFileRecord>>('/raw-files', input, options),
  listParsedRecords: (input: EvidenceListInput, options?: RequestOptions) =>
    get<Page<ParsedRecord>>('/parsed-records', input, options),
  listQcResults: (
    input: EvidenceListInput & { result?: string },
    options?: RequestOptions,
  ) => get<Page<QcResultRecord>>('/qc-results', input, options),
  listAlerts: (
    input: EvidenceListInput & { node_code: string; status?: string; severity?: string },
    options?: RequestOptions,
  ) => get<Page<AlertRecord>>('/alerts', input, options),
}
