import { mount } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { describe, expect, it } from 'vitest'

import LoadMoreButton from '../LoadMoreButton.vue'

function mountButton(props: {
  loading?: boolean
  loadingMore?: boolean
  hasMore?: boolean
}) {
  return mount(LoadMoreButton, {
    props: { loading: false, loadingMore: false, hasMore: true, ...props },
    global: { plugins: [ElementPlus] },
  })
}

describe('LoadMoreButton', () => {
  it('has_more=false 时整体隐藏', () => {
    const wrapper = mountButton({ hasMore: false })

    expect(wrapper.find('button').exists()).toBe(false)
  })

  it('点击发出 click 事件', async () => {
    const wrapper = mountButton({})

    await wrapper.find('button').trigger('click')

    expect(wrapper.emitted('click')).toHaveLength(1)
  })

  it('刷新中禁用', () => {
    const wrapper = mountButton({ loading: true })

    expect(wrapper.find('button').attributes('disabled')).toBeDefined()
  })

  it('追加中禁用并显示加载状态', () => {
    const wrapper = mountButton({ loadingMore: true })

    expect(wrapper.find('button').attributes('disabled')).toBeDefined()
    expect(wrapper.find('.is-loading').exists()).toBe(true)
  })
})
