import { mount, flushPromises } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { CanceledError } from 'axios'
import { beforeEach, describe, expect, it, vi } from 'vitest'

import { listNodes } from '@/api/management'
import type { Node, Page } from '@/api/types'

import NodeSelect from '../NodeSelect.vue'

vi.mock('@/api/management', () => ({
  listNodes: vi.fn(),
}))

const listNodesMock = vi.mocked(listNodes)

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

function pageOf(items: Node[], hasMore: boolean, nextCursor: string | null): Page<Node> {
  return { items, next_cursor: nextCursor, has_more: hasMore }
}

function mountSelect(modelValue: string | null = null) {
  return mount(NodeSelect, {
    props: { modelValue },
    global: { plugins: [ElementPlus] },
  })
}

beforeEach(() => {
  listNodesMock.mockReset()
})

describe('NodeSelect', () => {
  it('挂载后逐页取全节点列表作为选项', async () => {
    listNodesMock
      .mockResolvedValueOnce(
        pageOf([nodeOf('LAB-01'), nodeOf('LAB-02')], true, 'cursor-1'),
      )
      .mockResolvedValueOnce(pageOf([nodeOf('LAB-03')], false, null))

    const wrapper = mountSelect()
    const vm = wrapper.vm as unknown as { options: Node[] }
    await flushPromises()

    expect(listNodesMock).toHaveBeenCalledTimes(2)
    expect(listNodesMock).toHaveBeenNthCalledWith(1, {
      limit: 100,
      cursor: undefined,
    })
    expect(listNodesMock).toHaveBeenNthCalledWith(2, {
      limit: 100,
      cursor: 'cursor-1',
    })
    expect(vm.options.map((node) => node.node_code)).toEqual([
      'LAB-01',
      'LAB-02',
      'LAB-03',
    ])
  })

  it('选择节点发出 update:modelValue', async () => {
    listNodesMock.mockResolvedValue(pageOf([nodeOf('LAB-01')], false, null))
    const wrapper = mountSelect()
    await flushPromises()

    const select = wrapper.findComponent({ name: 'ElSelect' })
    select.vm.$emit('update:modelValue', 'LAB-01')
    await flushPromises()

    expect(wrapper.emitted('update:modelValue')?.[0]).toEqual(['LAB-01'])
  })

  it('加载失败进入错误状态并可重试', async () => {
    listNodesMock.mockRejectedValueOnce(new Error('boom'))
    const wrapper = mountSelect()
    await flushPromises()

    const vm = wrapper.vm as unknown as { error: unknown }
    expect(vm.error).not.toBeNull()
    expect(wrapper.text()).toContain('节点列表加载失败')

    listNodesMock.mockResolvedValueOnce(pageOf([nodeOf('LAB-01')], false, null))
    await wrapper.find('.node-select__error button').trigger('click')
    await flushPromises()

    expect(vm.error).toBeNull()
    const options = (wrapper.vm as unknown as { options: Node[] }).options
    expect(options).toHaveLength(1)
  })

  it('取消异常不进入错误状态', async () => {
    listNodesMock.mockRejectedValue(new CanceledError())
    const wrapper = mountSelect()
    await flushPromises()

    const vm = wrapper.vm as unknown as { error: unknown }
    expect(vm.error).toBeNull()
    expect(wrapper.text()).not.toContain('节点列表加载失败')
  })
})
