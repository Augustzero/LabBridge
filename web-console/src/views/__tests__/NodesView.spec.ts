import { mount, flushPromises } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createMemoryHistory, createRouter, type Router } from 'vue-router'

import { ApiError } from '@/api/http'
import { listNodes } from '@/api/management'
import type { Node, Page } from '@/api/types'
import { routes } from '@/router'

import NodesView from '../NodesView.vue'

vi.mock('@/api/management', () => ({
  listNodes: vi.fn(),
}))

const listNodesMock = vi.mocked(listNodes)

function nodeOf(code: string, status: 'online' | 'offline' = 'online'): Node {
  return {
    id: `id-${code}`,
    node_code: code,
    name: `节点 ${code}`,
    agent_version: '0.1.0',
    stored_status: status,
    effective_status: status,
    last_heartbeat_at: '2026-01-02T03:04:05Z',
    created_at: null,
    updated_at: null,
  }
}

function pageOf(items: Node[], hasMore: boolean, nextCursor: string | null): Page<Node> {
  return { items, next_cursor: nextCursor, has_more: hasMore }
}

async function mountView(path = '/nodes'): Promise<{
  wrapper: ReturnType<typeof mount>
  router: Router
}> {
  const router = createRouter({ history: createMemoryHistory(), routes })
  await router.push(path)
  await router.isReady()
  const wrapper = mount(NodesView, {
    global: { plugins: [router, ElementPlus] },
  })
  await flushPromises()
  return { wrapper, router }
}

beforeEach(() => {
  listNodesMock.mockReset()
})

describe('NodesView', () => {
  it('挂载后加载节点列表并渲染行', async () => {
    listNodesMock.mockResolvedValue(pageOf([nodeOf('LAB-01')], false, null))

    const { wrapper } = await mountView()

    expect(listNodesMock).toHaveBeenCalledTimes(1)
    expect(wrapper.text()).toContain('LAB-01')
    expect(wrapper.text()).toContain('节点 LAB-01')
    expect(wrapper.text()).toContain('在线')
    expect(wrapper.text()).toContain('2026-01-02 03:04:05 UTC')
  })

  it('从 URL 还原 status 筛选并携带到首屏请求', async () => {
    listNodesMock.mockResolvedValue(pageOf([], false, null))

    await mountView('/nodes?status=offline')

    expect(listNodesMock).toHaveBeenCalledWith(
      { status: 'offline', limit: undefined, cursor: undefined },
      expect.anything(),
    )
  })

  it('切换状态筛选刷新列表并同步 URL', async () => {
    listNodesMock.mockResolvedValue(pageOf([nodeOf('LAB-01')], false, null))
    const { wrapper, router } = await mountView()

    const group = wrapper.findComponent({ name: 'ElRadioGroup' })
    group.vm.$emit('change', 'online')
    await flushPromises()

    expect(listNodesMock).toHaveBeenLastCalledWith(
      { status: 'online', limit: undefined, cursor: undefined },
      expect.anything(),
    )
    expect(router.currentRoute.value.query.status).toBe('online')
  })

  it('清除筛选后 URL 移除 status 参数', async () => {
    listNodesMock.mockResolvedValue(pageOf([], false, null))
    const { wrapper, router } = await mountView('/nodes?status=online')

    const group = wrapper.findComponent({ name: 'ElRadioGroup' })
    group.vm.$emit('change', '')
    await flushPromises()

    expect(listNodesMock).toHaveBeenLastCalledWith(
      { status: undefined, limit: undefined, cursor: undefined },
      expect.anything(),
    )
    expect(router.currentRoute.value.query.status).toBeUndefined()
  })

  it('has_more 时加载更多追加下一页', async () => {
    listNodesMock
      .mockResolvedValueOnce(pageOf([nodeOf('LAB-01')], true, 'cursor-1'))
      .mockResolvedValueOnce(pageOf([nodeOf('LAB-02')], false, null))
    const { wrapper } = await mountView()

    await wrapper.find('.load-more button').trigger('click')
    await flushPromises()

    expect(listNodesMock).toHaveBeenLastCalledWith(
      { status: undefined, limit: undefined, cursor: 'cursor-1' },
      expect.anything(),
    )
    expect(wrapper.text()).toContain('LAB-01')
    expect(wrapper.text()).toContain('LAB-02')
  })

  it('加载失败显示错误横幅，重试成功后恢复', async () => {
    listNodesMock.mockRejectedValueOnce(new ApiError('network', 'boom'))
    const { wrapper } = await mountView()

    expect(wrapper.text()).toContain('网络连接失败')
    expect(wrapper.text()).toContain('重试')

    listNodesMock.mockResolvedValueOnce(pageOf([nodeOf('LAB-01')], false, null))
    await wrapper.find('.error-banner button').trigger('click')
    await flushPromises()

    expect(wrapper.text()).toContain('LAB-01')
  })

  it('点击行进入节点详情', async () => {
    listNodesMock.mockResolvedValue(pageOf([nodeOf('LAB-01')], false, null))
    const { wrapper, router } = await mountView()

    await wrapper.find('tbody tr').trigger('click')
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/nodes/LAB-01')
  })
})
