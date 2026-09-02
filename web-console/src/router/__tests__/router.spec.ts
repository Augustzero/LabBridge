import { describe, expect, it } from 'vitest'
import { createMemoryHistory, createRouter, type Router } from 'vue-router'

import ConsoleLayout from '@/layouts/ConsoleLayout.vue'
import NodeDetailView from '@/views/NodeDetailView.vue'
import NodesView from '@/views/NodesView.vue'
import TasksView from '@/views/TasksView.vue'
import TaskRunsView from '@/views/TaskRunsView.vue'

import { routes } from '../index'

function buildRouter(): Router {
  return createRouter({ history: createMemoryHistory(), routes })
}

describe('router', () => {
  it("路径 '/' 重定向到 /nodes", async () => {
    const router = buildRouter()
    await router.push('/')
    await router.isReady()

    expect(router.currentRoute.value.path).toBe('/nodes')
  })

  it('三个业务路由可达并使用 ConsoleLayout', () => {
    const router = buildRouter()

    const cases = [
      { path: '/nodes', name: 'nodes', view: NodesView },
      { path: '/tasks', name: 'tasks', view: TasksView },
      { path: '/runs', name: 'runs', view: TaskRunsView },
    ] as const

    for (const item of cases) {
      const resolved = router.resolve(item.path)
      expect(resolved.matched).toHaveLength(2)
      expect(resolved.matched[0]?.components?.default).toBe(ConsoleLayout)
      expect(resolved.matched[1]?.components?.default).toBe(item.view)
      expect(resolved.name).toBe(item.name)
    }
  })

  it('节点详情路由携带 nodeCode 参数', () => {
    const router = buildRouter()

    const resolved = router.resolve('/nodes/LAB-01')

    expect(resolved.name).toBe('node-detail')
    expect(resolved.matched[1]?.components?.default).toBe(NodeDetailView)
    expect(resolved.params.nodeCode).toBe('LAB-01')
  })
})
