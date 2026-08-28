import { flushPromises, mount } from "@vue/test-utils";
import ElementPlus from "element-plus";
import { createMemoryHistory, createRouter } from "vue-router";

import { managementApi } from "@/api/management";
import appRouter from "./index";
import NodesView from "@/views/NodesView.vue";
import TaskRunsView from "@/views/TaskRunsView.vue";
import TasksView from "@/views/TasksView.vue";

describe("management routes", () => {
  beforeEach(() => {
    const emptyPage = { items: [], next_cursor: null, has_more: false };
    vi.spyOn(managementApi, "listNodes").mockResolvedValue(emptyPage);
    vi.spyOn(managementApi, "listTasks").mockResolvedValue(emptyPage);
    vi.spyOn(managementApi, "listTaskRuns").mockResolvedValue(emptyPage);
  });

  it.each([
    ["/nodes", "节点 · LabBridge"],
    ["/tasks", "任务 · LabBridge"],
    ["/task-runs", "运行历史 · LabBridge"],
  ])("使用中文页面标题 %s", async (path, title) => {
    await appRouter.push(path);
    await appRouter.isReady();
    expect(document.title).toBe(title);
  });

  it.each([
    ["/nodes", "节点"],
    ["/tasks?node_code=demo-node-001", "任务"],
    ["/task-runs?node_code=demo-node-001&task_id=12&run_id=34", "运行历史"],
  ])("可访问 %s 并保留定位 query", async (path, heading) => {
    const router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: "/nodes", component: NodesView },
        { path: "/tasks", component: TasksView },
        { path: "/task-runs", component: TaskRunsView },
      ],
    });
    await router.push(path);
    await router.isReady();

    const wrapper = mount(
      { template: "<router-view />" },
      {
        global: {
          plugins: [router, ElementPlus],
          stubs: { RunEvidenceDrawer: true },
        },
      },
    );
    await flushPromises();

    expect(wrapper.get("h2").text()).toBe(heading);
    expect(router.currentRoute.value.query).toEqual(
      expect.objectContaining(
        path.includes("node_code") ? { node_code: "demo-node-001" } : {},
      ),
    );
  });
});
