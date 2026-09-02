import { mount, flushPromises } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { defineComponent, h } from 'vue'
import { describe, expect, it } from 'vitest'
import { createMemoryHistory, createRouter, RouterView, type Router } from 'vue-router'

import { routes } from '@/router'

import ConsoleLayout from '../ConsoleLayout.vue'

// 与 App.vue 等价的根：直接挂载布局会让布局内 router-view 把布局再渲染一层
const Root = defineComponent({ render: () => h(RouterView) })

function mountLayout(): { wrapper: ReturnType<typeof mount>; router: Router } {
  const router = createRouter({ history: createMemoryHistory(), routes })
  const wrapper = mount(Root, {
    global: { plugins: [router, ElementPlus] },
  })
  return { wrapper, router }
}

// 布局以 el-menu router 模式导航；activeIndex 由路径前缀计算，
// 节点详情等子路径也应保持"节点管理"高亮
describe('ConsoleLayout', () => {
  it("访问 / 时重定向到 /nodes 并高亮节点管理", async () => {
    const { wrapper, router } = mountLayout()
    await router.push('/')
    await router.isReady()
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/nodes')
    const active = wrapper.findAll('.el-menu-item.is-active')
    expect(active).toHaveLength(1)
    expect(active[0]?.text()).toContain('节点管理')
  })

  it('点击导航项切换页面并更新高亮', async () => {
    const { wrapper, router } = mountLayout()
    await router.push('/nodes')
    await router.isReady()
    await flushPromises()

    const items = wrapper.findAll('.el-menu-item')
    const taskItem = items.find((item) => item.text().includes('任务管理'))
    await taskItem?.trigger('click')
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/tasks')
    const active = wrapper.findAll('.el-menu-item.is-active')
    expect(active).toHaveLength(1)
    expect(active[0]?.text()).toContain('任务管理')
    expect(wrapper.find('.console-header').text()).toContain('任务管理')
  })

  it('节点详情子路径保持节点管理高亮', async () => {
    const { wrapper, router } = mountLayout()
    await router.push('/nodes/LAB-01')
    await router.isReady()
    await flushPromises()

    const active = wrapper.findAll('.el-menu-item.is-active')
    expect(active).toHaveLength(1)
    expect(active[0]?.text()).toContain('节点管理')
    expect(wrapper.find('.console-header').text()).toContain('节点详情')
  })
})
