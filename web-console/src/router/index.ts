import { createRouter, createWebHistory } from 'vue-router'

import ConsoleLayout from '@/layouts/ConsoleLayout.vue'
import NodesView from '@/views/NodesView.vue'
import TasksView from '@/views/TasksView.vue'
import TaskRunsView from '@/views/TaskRunsView.vue'

// 三个业务 view 当前为占位组件；026R-03/04 替换为实际页面
export const routes = [
  { path: '/', redirect: '/nodes' },
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
        component: NodesView,
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

const router = createRouter({
  history: createWebHistory(),
  routes,
})

export default router
