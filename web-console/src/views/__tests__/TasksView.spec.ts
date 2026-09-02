import { mount, flushPromises } from '@vue/test-utils'
import ElementPlus, { ElMessageBox, type MessageBoxData } from 'element-plus'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createMemoryHistory, createRouter, type Router } from 'vue-router'

import { ApiError } from '@/api/http'
import { listNodes, listTasks, setTaskEnabled } from '@/api/management'
import type { Node, Page, Task } from '@/api/types'
import { routes } from '@/router'

import NodeSelect from '@/components/NodeSelect.vue'

import TasksView from '../TasksView.vue'

vi.mock('@/api/management', () => ({
  listNodes: vi.fn(),
  listTasks: vi.fn(),
  setTaskEnabled: vi.fn(),
}))

vi.mock('element-plus', async (importOriginal) => {
  const actual = await importOriginal<typeof import('element-plus')>()
  return { ...actual, ElMessageBox: { confirm: vi.fn() } }
})

const listNodesMock = vi.mocked(listNodes)
const listTasksMock = vi.mocked(listTasks)
const setTaskEnabledMock = vi.mocked(setTaskEnabled)
const confirmMock = vi.mocked(ElMessageBox.confirm)

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

function taskOf(id: string, enabled: boolean): Task {
  return {
    id,
    node_code: 'LAB-01',
    data_source_id: 'ds-1',
    name: `任务 ${id}`,
    task_type: 'file_collect',
    schedule_expr: '*/30 * * * *',
    parser_type: 'csv',
    qc_profile: 'default',
    enabled,
    qc_rule_ids: [],
    created_at: null,
    updated_at: null,
  }
}

function taskPageOf(tasks: Task[], hasMore = false): Page<Task> {
  return { items: tasks, next_cursor: null, has_more: hasMore }
}

async function mountView(path = '/tasks'): Promise<{
  wrapper: ReturnType<typeof mount>
  router: Router
}> {
  const router = createRouter({ history: createMemoryHistory(), routes })
  await router.push(path)
  await router.isReady()
  const wrapper = mount(TasksView, {
    global: { plugins: [router, ElementPlus] },
  })
  await flushPromises()
  return { wrapper, router }
}

beforeEach(() => {
  listNodesMock.mockReset()
  listTasksMock.mockReset()
  setTaskEnabledMock.mockReset()
  confirmMock.mockReset()
  listNodesMock.mockResolvedValue({
    items: [nodeOf('LAB-01'), nodeOf('LAB-02')],
    next_cursor: null,
    has_more: false,
  })
})

describe('TasksView', () => {
  it('未选节点时显示引导空态且不请求任务', async () => {
    const { wrapper } = await mountView()

    expect(listTasksMock).not.toHaveBeenCalled()
    expect(wrapper.text()).toContain('请先选择节点')
  })

  it('从 URL 还原 node 与 enabled 筛选并携带到首屏请求', async () => {
    listTasksMock.mockResolvedValue(taskPageOf([taskOf('t-1', true)]))

    const { wrapper } = await mountView('/tasks?node=LAB-01&enabled=true')

    expect(listTasksMock).toHaveBeenCalledWith(
      { nodeCode: 'LAB-01', enabled: true, limit: undefined, cursor: undefined },
      expect.anything(),
    )
    expect(wrapper.text()).toContain('任务 t-1')
  })

  it('选择节点后加载任务并同步 URL', async () => {
    listTasksMock.mockResolvedValue(taskPageOf([taskOf('t-1', true)]))
    const { wrapper, router } = await mountView()

    wrapper.findComponent(NodeSelect).vm.$emit('update:modelValue', 'LAB-01')
    await flushPromises()

    expect(listTasksMock).toHaveBeenCalledWith(
      { nodeCode: 'LAB-01', enabled: undefined, limit: undefined, cursor: undefined },
      expect.anything(),
    )
    expect(router.currentRoute.value.query).toEqual({ node: 'LAB-01' })
    expect(wrapper.text()).toContain('任务 t-1')
  })

  it('切换启用筛选刷新请求并同步 URL', async () => {
    listTasksMock.mockResolvedValue(taskPageOf([]))
    const { wrapper, router } = await mountView('/tasks?node=LAB-01')

    const group = wrapper.findComponent({ name: 'ElRadioGroup' })
    group.vm.$emit('change', 'false')
    await flushPromises()

    expect(listTasksMock).toHaveBeenLastCalledWith(
      { nodeCode: 'LAB-01', enabled: false, limit: undefined, cursor: undefined },
      expect.anything(),
    )
    expect(router.currentRoute.value.query).toEqual({
      node: 'LAB-01',
      enabled: 'false',
    })
  })

  it('确认后启停成功，以响应体更新行数据', async () => {
    confirmMock.mockResolvedValue('confirm' as MessageBoxData)
    listTasksMock.mockResolvedValue(taskPageOf([taskOf('t-1', true)]))
    setTaskEnabledMock.mockResolvedValue(taskOf('t-1', false))
    const { wrapper } = await mountView('/tasks?node=LAB-01')

    wrapper.findComponent({ name: 'ElSwitch' }).vm.$emit('change', false)
    await flushPromises()

    expect(confirmMock).toHaveBeenCalledTimes(1)
    expect(setTaskEnabledMock).toHaveBeenCalledWith('t-1', false)
    expect(wrapper.text()).toContain('已停用')
  })

  it('启停失败时行状态不变并展示错误横幅', async () => {
    confirmMock.mockResolvedValue('confirm' as MessageBoxData)
    listTasksMock.mockResolvedValue(taskPageOf([taskOf('t-1', true)]))
    setTaskEnabledMock.mockRejectedValue(
      new ApiError('conflict', 'task is running'),
    )
    const { wrapper } = await mountView('/tasks?node=LAB-01')

    wrapper.findComponent({ name: 'ElSwitch' }).vm.$emit('change', false)
    await flushPromises()

    expect(wrapper.text()).toContain('操作与当前状态冲突')
    expect(wrapper.text()).toContain('task is running')
    expect(wrapper.text()).toContain('已启用')
  })

  it('取消确认框不发起启停请求', async () => {
    confirmMock.mockRejectedValue('cancel')
    listTasksMock.mockResolvedValue(taskPageOf([taskOf('t-1', true)]))
    const { wrapper } = await mountView('/tasks?node=LAB-01')

    wrapper.findComponent({ name: 'ElSwitch' }).vm.$emit('change', false)
    await flushPromises()

    expect(setTaskEnabledMock).not.toHaveBeenCalled()
    expect(wrapper.text()).toContain('已启用')
  })
})
