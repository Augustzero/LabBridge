import { createRouter, createWebHistory } from "vue-router";

import NodesView from "@/views/NodesView.vue";
import TaskRunsView from "@/views/TaskRunsView.vue";
import TasksView from "@/views/TasksView.vue";

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: "/", redirect: "/nodes" },
    {
      path: "/nodes",
      name: "nodes",
      component: NodesView,
      meta: { title: "节点" },
    },
    {
      path: "/tasks",
      name: "tasks",
      component: TasksView,
      meta: { title: "任务" },
    },
    {
      path: "/task-runs",
      name: "task-runs",
      component: TaskRunsView,
      meta: { title: "运行历史" },
    },
  ],
});

router.afterEach((route) => {
  const title =
    typeof route.meta.title === "string" ? route.meta.title : "管理台";
  document.title = `${title} · LabBridge`;
});

export default router;
