import { createRouter, createWebHistory, type Router } from 'vue-router'

import { currentToken } from '@/api/credentials'
import ConsoleLayout from '@/layouts/ConsoleLayout.vue'
import AuthView from '@/views/AuthView.vue'
import NodeDetailView from '@/views/NodeDetailView.vue'
import NodesView from '@/views/NodesView.vue'
import TasksView from '@/views/TasksView.vue'
import TaskRunsView from '@/views/TaskRunsView.vue'

export const routes = [
  { path: '/', redirect: '/nodes' },
  {
    path: '/auth',
    name: 'auth',
    component: AuthView,
    // 唯一的公开路由：没有凭据时的落脚点，业务页面必须持 token 才能进
    meta: { title: '访问凭据', public: true },
  },
  {
    path: '/',
    component: ConsoleLayout,
    children: [
      {
        path: 'nodes',
        name: 'nodes',
        component: NodesView,
        meta: { title: '节点管理' },
      },
      {
        path: 'nodes/:nodeCode',
        name: 'node-detail',
        component: NodeDetailView,
        meta: { title: '节点详情' },
      },
      {
        path: 'tasks',
        name: 'tasks',
        component: TasksView,
        meta: { title: '任务管理' },
      },
      {
        path: 'runs',
        name: 'runs',
        component: TaskRunsView,
        meta: { title: '运行历史' },
      },
    ],
  },
]

// 路由守卫单独导出：默认 router 装配时挂上，测试自建 router 时可按需复用
export function setupAuthGuard(router: Router): void {
  router.beforeEach((to) => {
    if (to.meta.public === true || currentToken() != null) {
      return true
    }
    // 没有凭据的任何业务导航都改去输入页，带上原目标便于登录后回来
    return { name: 'auth', query: { redirect: to.fullPath } }
  })
}

// 重定向目标只接受应用内路径（单个 / 开头，排除 // 协议相对地址），
// 其他外来值一律回节点页
export function sanitizeRedirect(value: unknown): string {
  if (typeof value === 'string' && value.startsWith('/') && !value.startsWith('//')) {
    return value
  }
  return '/nodes'
}

const router = createRouter({
  history: createWebHistory(),
  routes,
})
setupAuthGuard(router)

export default router
