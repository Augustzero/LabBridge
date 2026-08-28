import { apiClient } from './client'
import { managementApi } from './management'

describe('management API routes', () => {
  it('把 keyset 与筛选参数原样交给既有管理路由', async () => {
    const get = vi.spyOn(apiClient, 'get').mockResolvedValue({
      items: [],
      next_cursor: null,
      has_more: false,
    })

    await managementApi.listTaskRuns({
      node_code: 'demo-node-001',
      task_id: '9007199254740993',
      status: 'succeeded',
      limit: 20,
      cursor: 'cursor-value',
    })

    expect(get).toHaveBeenCalledWith('/task-runs', {
      params: {
        node_code: 'demo-node-001',
        task_id: '9007199254740993',
        status: 'succeeded',
        limit: 20,
        cursor: 'cursor-value',
      },
      signal: undefined,
    })
  })

  it('对路径段编码并保留字符串 ID', async () => {
    const get = vi.spyOn(apiClient, 'get').mockResolvedValue({
      id: '9007199254740993',
      task_id: '41',
    })

    const result = await managementApi.getTaskRun(
      '9007199254740993',
      'node/with space',
    )

    expect(get).toHaveBeenCalledWith('/task-runs/9007199254740993', {
      params: { node_code: 'node/with space' },
      signal: undefined,
    })
    expect(result.id).toBe('9007199254740993')
  })
})
