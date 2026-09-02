import { mount, flushPromises } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createMemoryHistory, createRouter, type Router } from 'vue-router'

import { ApiError } from '@/api/http'
import { findNode, listDataSources } from '@/api/management'
import type { DataSource, NodeSummary, Page, TaskRun } from '@/api/types'
import { routes } from '@/router'

import NodeDetailView from '../NodeDetailView.vue'

vi.mock('@/api/management', () => ({
  findNode: vi.fn(),
  listDataSources: vi.fn(),
}))

const findNodeMock = vi.mocked(findNode)
const listDataSourcesMock = vi.mocked(listDataSources)

function summaryOf(overrides: Partial<NodeSummary> = {}): NodeSummary {
  return {
    id: 'node-1',
    node_code: 'LAB-01',
    name: '实验室 1 号节点',
    agent_version: '0.1.0',
    stored_status: 'online',
    effective_status: 'online',
    last_heartbeat_at: '2026-01-02T03:04:05Z',
    created_at: '2026-01-01T00:00:00Z',
    updated_at: null,
    enabled_task_count: 2,
    disabled_task_count: 1,
    open_alert_count: 3,
    latest_task_run: null,
    ...overrides,
  }
}

function sourceOf(name: string): DataSource {
  return {
    id: `ds-${name}`,
    node_code: 'LAB-01',
    source_type: 'local_directory',
    name,
    config: {},
    enabled: true,
    created_at: null,
    updated_at: null,
  }
}

function runOf(): TaskRun {
  return {
    id: 'run-9',
    task_id: 'task-1',
    node_code: 'LAB-01',
    status: 'succeeded',
    started_at: '2026-01-02T03:00:00Z',
    finished_at: '2026-01-02T03:00:10Z',
    scheduled_for: null,
    trigger_type: 'schedule',
    execution_key: null,
    items_total: 1,
    items_success: 1,
    items_failed: 0,
    error_summary: null,
  }
}

function pageOf(items: DataSource[]): Page<DataSource> {
  return { items, next_cursor: null, has_more: false }
}

async function mountView(path = '/nodes/LAB-01'): Promise<{
  wrapper: ReturnType<typeof mount>
  router: Router
}> {
  const router = createRouter({ history: createMemoryHistory(), routes })
  await router.push(path)
  await router.isReady()
  const wrapper = mount(NodeDetailView, {
    global: { plugins: [router, ElementPlus] },
  })
  await flushPromises()
  return { wrapper, router }
}

beforeEach(() => {
  findNodeMock.mockReset()
  listDataSourcesMock.mockReset()
})

describe('NodeDetailView', () => {
  it('加载节点摘要并渲染基本信息、计数与最近运行', async () => {
    findNodeMock.mockResolvedValue(
      summaryOf({ latest_task_run: runOf() }),
    )
    listDataSourcesMock.mockResolvedValue(pageOf([]))

    const { wrapper } = await mountView()

    expect(findNodeMock).toHaveBeenCalledWith('LAB-01')
    expect(wrapper.text()).toContain('实验室 1 号节点')
    expect(wrapper.text()).toContain('启用任务')
    expect(wrapper.text()).toContain('未处理告警')
    expect(wrapper.text()).toContain('run-9')
    expect(wrapper.text()).toContain('成功')
    expect(wrapper.text()).toContain('2026-01-02 03:04:05 UTC')
  })

  it('无最近运行时显示占位文案', async () => {
    findNodeMock.mockResolvedValue(summaryOf())
    listDataSourcesMock.mockResolvedValue(pageOf([]))

    const { wrapper } = await mountView()

    expect(wrapper.text()).toContain('暂无运行记录')
  })

  it('数据源折叠表格渲染类型、名称与启用状态', async () => {
    findNodeMock.mockResolvedValue(summaryOf())
    listDataSourcesMock.mockResolvedValue(pageOf([sourceOf('现场目录')]))

    const { wrapper } = await mountView()

    expect(listDataSourcesMock).toHaveBeenCalledWith({
      nodeCode: 'LAB-01',
      limit: 100,
      cursor: undefined,
    })
    expect(wrapper.text()).toContain('数据源（1）')
    // 折叠面板内容需展开后可见
    await wrapper.find('.el-collapse-item__header').trigger('click')
    await flushPromises()
    expect(wrapper.text()).toContain('现场目录')
    expect(wrapper.text()).toContain('已启用')
  })

  it('节点查询 404 显示错误横幅并可重试', async () => {
    findNodeMock.mockRejectedValueOnce(new ApiError('not_found', 'not found'))
    listDataSourcesMock.mockResolvedValue(pageOf([]))

    const { wrapper } = await mountView()

    expect(wrapper.text()).toContain('请求的资源不存在')

    findNodeMock.mockResolvedValueOnce(summaryOf())
    await wrapper.find('.error-banner button').trigger('click')
    await flushPromises()

    expect(wrapper.text()).toContain('实验室 1 号节点')
  })

  it('入口链接携带 node 参数跳转任务页与运行页', async () => {
    findNodeMock.mockResolvedValue(summaryOf())
    listDataSourcesMock.mockResolvedValue(pageOf([]))

    const { wrapper, router } = await mountView()

    const buttons = wrapper.findAll('.node-detail__links button')
    await buttons[0]?.trigger('click')
    await flushPromises()
    expect(router.currentRoute.value.fullPath).toBe('/tasks?node=LAB-01')

    await buttons[1]?.trigger('click')
    await flushPromises()
    expect(router.currentRoute.value.fullPath).toBe('/runs?node=LAB-01')
  })
})
