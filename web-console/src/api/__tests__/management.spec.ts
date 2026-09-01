import { beforeEach, describe, expect, it, vi } from 'vitest'

import { request } from '@/api/http'
import type { Page } from '@/api/types'

import {
  findNode,
  findTaskRun,
  listAlerts,
  listDataSources,
  listNodes,
  listParsedRecords,
  listQcResults,
  listQcRules,
  listRawFiles,
  listTaskRuns,
  listTasks,
  setTaskEnabled,
} from '../management'

vi.mock('@/api/http', () => ({
  request: vi.fn(),
}))

const mockedRequest = vi.mocked(request)

function lastCallConfig(): {
  method?: string
  url?: string
  params?: Record<string, string>
  data?: unknown
} {
  const [config] = mockedRequest.mock.calls[mockedRequest.mock.calls.length - 1]
  return config as {
    method?: string
    url?: string
    params?: Record<string, string>
    data?: unknown
  }
}

beforeEach(() => {
  mockedRequest.mockReset()
  mockedRequest.mockResolvedValue({
    items: [],
    next_cursor: null,
    has_more: false,
  } satisfies Page<never>)
})

describe('查询端点 URL 与 query 构造', () => {
  it('listNodes：status 透传，未设置的参数不出现在 query', async () => {
    await listNodes({ status: 'online' })
    const config = lastCallConfig()
    expect(config.method).toBe('get')
    expect(config.url).toBe('/api/v1/nodes')
    expect(config.params).toEqual({ status: 'online' })
  })

  it('listNodes：limit 与 cursor 以字符串透传', async () => {
    await listNodes({ limit: 20, cursor: 'node-7' })
    expect(lastCallConfig().params).toEqual({ limit: '20', cursor: 'node-7' })
  })

  it('listNodes：全部可选参数缺省时发送空 query', async () => {
    await listNodes({})
    expect(lastCallConfig().params).toEqual({})
  })

  it('findNode：路径拼接节点编码', async () => {
    await findNode('node-a')
    const config = lastCallConfig()
    expect(config.url).toBe('/api/v1/nodes/node-a')
    expect(config.params).toBeUndefined()
  })

  it('listDataSources：必填 node_code，enabled 布尔转字符串', async () => {
    await listDataSources({ nodeCode: 'node-a', enabled: false })
    expect(lastCallConfig().params).toEqual({
      node_code: 'node-a',
      enabled: 'false',
    })
  })

  it('listQcRules：enabled 缺省时不发送该参数', async () => {
    await listQcRules({})
    expect(lastCallConfig().params).toEqual({})
  })

  it('listTasks：必填 node_code', async () => {
    await listTasks({ nodeCode: 'node-a', enabled: true })
    expect(lastCallConfig().url).toBe('/api/v1/tasks')
    expect(lastCallConfig().params).toEqual({
      node_code: 'node-a',
      enabled: 'true',
    })
  })

  it('listTaskRuns：可选 task_id/status 仅在设置时发送', async () => {
    await listTaskRuns({ nodeCode: 'node-a' })
    expect(lastCallConfig().params).toEqual({ node_code: 'node-a' })

    await listTaskRuns({
      nodeCode: 'node-a',
      taskId: 'task-1',
      status: 'succeeded',
    })
    expect(lastCallConfig().params).toEqual({
      node_code: 'node-a',
      task_id: 'task-1',
      status: 'succeeded',
    })
  })

  it('findTaskRun：run id 进路径，node_code 进 query', async () => {
    await findTaskRun('run-9', 'node-a')
    const config = lastCallConfig()
    expect(config.url).toBe('/api/v1/task-runs/run-9')
    expect(config.params).toEqual({ node_code: 'node-a' })
  })

  it('listRawFiles / listParsedRecords：必填 task_run_id', async () => {
    await listRawFiles({ taskRunId: 'run-9' })
    expect(lastCallConfig().url).toBe('/api/v1/raw-files')
    expect(lastCallConfig().params).toEqual({ task_run_id: 'run-9' })

    await listParsedRecords({ taskRunId: 'run-9', cursor: 'p-2' })
    expect(lastCallConfig().url).toBe('/api/v1/parsed-records')
    expect(lastCallConfig().params).toEqual({
      task_run_id: 'run-9',
      cursor: 'p-2',
    })
  })

  it('listQcResults：result 仅在设置时发送', async () => {
    await listQcResults({ taskRunId: 'run-9', result: 'failed' })
    expect(lastCallConfig().params).toEqual({
      task_run_id: 'run-9',
      result: 'failed',
    })
  })

  it('listAlerts：node_code 必填，其余可选参数按需发送', async () => {
    await listAlerts({
      nodeCode: 'node-a',
      taskRunId: 'run-9',
      status: 'open',
      severity: 'warning',
    })
    expect(lastCallConfig().url).toBe('/api/v1/alerts')
    expect(lastCallConfig().params).toEqual({
      node_code: 'node-a',
      task_run_id: 'run-9',
      status: 'open',
      severity: 'warning',
    })
  })
})

describe('写端点', () => {
  it('setTaskEnabled：PATCH 且 body 仅含 enabled', async () => {
    await setTaskEnabled('task-1', true)
    const config = lastCallConfig()
    expect(config.method).toBe('patch')
    expect(config.url).toBe('/api/v1/tasks/task-1')
    expect(config.data).toEqual({ enabled: true })

    await setTaskEnabled('task-1', false)
    expect(lastCallConfig().data).toEqual({ enabled: false })
  })
})

describe('signal 透传', () => {
  it('AbortSignal 原样传给 http 层', async () => {
    const controller = new AbortController()
    await listNodes({}, controller.signal)
    expect(mockedRequest.mock.calls[0][1]).toBe(controller.signal)
  })
})
