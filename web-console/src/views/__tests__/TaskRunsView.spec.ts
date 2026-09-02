import { flushPromises, mount } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createMemoryHistory, createRouter, type Router } from 'vue-router'

import {
  findTaskRun,
  listAlerts,
  listNodes,
  listParsedRecords,
  listQcResults,
  listRawFiles,
  listTaskRuns,
  listTasks,
} from '@/api/management'
import type { Node, Page, Task, TaskRunWithStale } from '@/api/types'
import RunEvidenceDrawer from '@/components/RunEvidenceDrawer.vue'
import { routes } from '@/router'

import NodeSelect from '@/components/NodeSelect.vue'

import TaskRunsView from '../TaskRunsView.vue'

vi.mock('@/api/management', () => ({
  listNodes: vi.fn(),
  listTasks: vi.fn(),
  listTaskRuns: vi.fn(),
  findTaskRun: vi.fn(),
  listRawFiles: vi.fn(),
  listParsedRecords: vi.fn(),
  listQcResults: vi.fn(),
  listAlerts: vi.fn(),
}))

const listNodesMock = vi.mocked(listNodes)
const listTasksMock = vi.mocked(listTasks)
const listTaskRunsMock = vi.mocked(listTaskRuns)
const findTaskRunMock = vi.mocked(findTaskRun)
const listRawFilesMock = vi.mocked(listRawFiles)
const listParsedRecordsMock = vi.mocked(listParsedRecords)
const listQcResultsMock = vi.mocked(listQcResults)
const listAlertsMock = vi.mocked(listAlerts)

function nodeOf(code: string): Node {
  return {
    id: `id-${code}`,
    node_code: code,
    name: `节点 ${code}`,
    agent_version: '0.1.0',
    stored_status: 'online',
    effective_status: 'online',
    last_heartbeat_at: null,
    created_at: null,
    updated_at: null,
  }
}

function taskOf(id: string): Task {
  return {
    id,
    node_code: 'LAB-01',
    data_source_id: 'ds-1',
    name: `任务 ${id}`,
    task_type: 'file_collect',
    schedule_expr: '*/30 * * * *',
    parser_type: 'csv',
    qc_profile: 'default',
    enabled: true,
    qc_rule_ids: [],
    created_at: null,
    updated_at: null,
  }
}

function runOf(id: string, overrides: Partial<TaskRunWithStale> = {}): TaskRunWithStale {
  return {
    id,
    task_id: '7',
    node_code: 'LAB-01',
    status: 'succeeded',
    started_at: '2026-09-02T08:00:00Z',
    finished_at: '2026-09-02T08:00:02Z',
    scheduled_for: null,
    trigger_type: 'schedule',
    execution_key: null,
    items_total: 2,
    items_success: 2,
    items_failed: 0,
    error_summary: null,
    stale: false,
    stale_after_seconds: 3600,
    ...overrides,
  }
}

function pageOf<T>(items: T[], hasMore = false): Page<T> {
  return { items, next_cursor: null, has_more: hasMore }
}

async function mountView(path = '/runs'): Promise<{
  wrapper: ReturnType<typeof mount>
  router: Router
}> {
  const router = createRouter({ history: createMemoryHistory(), routes })
  await router.push(path)
  await router.isReady()
  const wrapper = mount(TaskRunsView, {
    global: { plugins: [router, ElementPlus] },
  })
  await flushPromises()
  return { wrapper, router }
}

beforeEach(() => {
  listNodesMock.mockReset()
  listTasksMock.mockReset()
  listTaskRunsMock.mockReset()
  findTaskRunMock.mockReset()
  listNodesMock.mockResolvedValue(pageOf([nodeOf('LAB-01'), nodeOf('LAB-02')]))
  listTasksMock.mockResolvedValue(pageOf([taskOf('7')]))
  listTaskRunsMock.mockResolvedValue(pageOf([runOf('5')]))
  findTaskRunMock.mockResolvedValue({
    ...runOf('5'),
    raw_file_count: 1,
    parsed_record_count: 2,
    qc_result_count: 4,
    alert_count: 1,
  })
  listRawFilesMock.mockResolvedValue(pageOf([]))
  listParsedRecordsMock.mockResolvedValue(pageOf([]))
  listQcResultsMock.mockResolvedValue(pageOf([]))
  listAlertsMock.mockResolvedValue(pageOf([]))
})

describe('TaskRunsView', () => {
  it('未选节点时显示引导空态且不请求运行记录与任务', async () => {
    const { wrapper } = await mountView()

    expect(listTaskRunsMock).not.toHaveBeenCalled()
    expect(listTasksMock).not.toHaveBeenCalled()
    expect(wrapper.text()).toContain('请先选择节点')
  })

  it('从 URL 还原 node/task/status 筛选并携带到首屏请求', async () => {
    listTaskRunsMock.mockResolvedValue(pageOf([runOf('5')]))
    const { wrapper } = await mountView('/runs?node=LAB-01&task=7&status=failed')

    expect(listTaskRunsMock).toHaveBeenCalledWith(
      {
        nodeCode: 'LAB-01',
        taskId: '7',
        status: 'failed',
        limit: undefined,
        cursor: undefined,
      },
      expect.anything(),
    )
    expect(wrapper.text()).toContain('任务 7')
  })

  it('选择节点后加载运行与任务下拉选项并同步 URL', async () => {
    const { wrapper, router } = await mountView()

    wrapper.findComponent(NodeSelect).vm.$emit('update:modelValue', 'LAB-01')
    await flushPromises()

    expect(listTaskRunsMock).toHaveBeenCalledWith(
      {
        nodeCode: 'LAB-01',
        taskId: undefined,
        status: undefined,
        limit: undefined,
        cursor: undefined,
      },
      expect.anything(),
    )
    // 任务下拉取全循环与 NodeSelect/NodeDetailView 一致，不携带 AbortSignal
    expect(listTasksMock).toHaveBeenCalledWith({
      nodeCode: 'LAB-01',
      limit: 100,
      cursor: undefined,
    })
    expect(router.currentRoute.value.query).toEqual({ node: 'LAB-01' })
  })

  it('切换任务筛选刷新请求并同步 URL', async () => {
    const { wrapper, router } = await mountView('/runs?node=LAB-01')

    // 页面有两个 ElSelect：第一个在 NodeSelect 内，第二个才是任务筛选
    const selects = wrapper.findAllComponents({ name: 'ElSelect' })
    selects[1]?.vm.$emit('update:modelValue', '7')
    await flushPromises()

    expect(listTaskRunsMock).toHaveBeenLastCalledWith(
      {
        nodeCode: 'LAB-01',
        taskId: '7',
        status: undefined,
        limit: undefined,
        cursor: undefined,
      },
      expect.anything(),
    )
    expect(router.currentRoute.value.query).toEqual({
      node: 'LAB-01',
      task: '7',
    })
  })

  it('URL 中遗留的任务筛选不在该节点任务列表时被清除', async () => {
    listTasksMock.mockResolvedValue(pageOf([taskOf('7')]))
    const { router } = await mountView('/runs?node=LAB-01&task=999')

    expect(router.currentRoute.value.query).toEqual({ node: 'LAB-01' })
  })

  it('切换状态筛选刷新请求并同步 URL', async () => {
    const { wrapper, router } = await mountView('/runs?node=LAB-01')

    wrapper
      .findComponent({ name: 'ElRadioGroup' })
      .vm.$emit('change', 'succeeded')
    await flushPromises()

    expect(listTaskRunsMock).toHaveBeenLastCalledWith(
      {
        nodeCode: 'LAB-01',
        taskId: undefined,
        status: 'succeeded',
        limit: undefined,
        cursor: undefined,
      },
      expect.anything(),
    )
    expect(router.currentRoute.value.query).toEqual({
      node: 'LAB-01',
      status: 'succeeded',
    })
  })

  it('stale 运行在状态列同时显示状态与已超时徽标', async () => {
    listTaskRunsMock.mockResolvedValue(
      pageOf([runOf('5', { stale: true, status: 'failed' })]),
    )
    const { wrapper } = await mountView('/runs?node=LAB-01')

    expect(wrapper.text()).toContain('失败')
    expect(wrapper.text()).toContain('已超时')
  })

  it('行点击打开证据抽屉并携带运行与节点信息', async () => {
    const { wrapper } = await mountView('/runs?node=LAB-01')

    wrapper
      .findComponent({ name: 'ElTable' })
      .vm.$emit('row-click', runOf('5'))
    await flushPromises()

    const drawer = wrapper.findComponent(RunEvidenceDrawer)
    expect(drawer.props('runId')).toBe('5')
    expect(drawer.props('nodeCode')).toBe('LAB-01')
    expect(findTaskRunMock).toHaveBeenCalledWith('5', 'LAB-01', expect.anything())
  })

  it('抽屉关闭事件将 runId 复位为空', async () => {
    const { wrapper } = await mountView('/runs?node=LAB-01')

    wrapper
      .findComponent({ name: 'ElTable' })
      .vm.$emit('row-click', runOf('5'))
    await flushPromises()

    wrapper.findComponent(RunEvidenceDrawer).vm.$emit('close')
    await flushPromises()

    expect(wrapper.findComponent(RunEvidenceDrawer).props('runId')).toBeNull()
  })
})
