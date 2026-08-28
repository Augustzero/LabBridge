import { flushPromises, mount } from "@vue/test-utils";
import { createMemoryHistory, createRouter } from "vue-router";

import { managementApi } from "@/api/management";
import ErrorNotice from "@/components/ErrorNotice.vue";
import RunEvidenceDrawer from "@/components/RunEvidenceDrawer.vue";
import NodesView from "./NodesView.vue";
import TaskRunsView from "./TaskRunsView.vue";
import TasksView from "./TasksView.vue";

const emptyPage = { items: [], next_cursor: null, has_more: false };

function routerAt(path: string) {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: "/tasks", component: TasksView },
      { path: "/task-runs", component: TaskRunsView },
    ],
  });
  return router
    .push(path)
    .then(() => router.isReady())
    .then(() => router);
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function nodeSummary(nodeCode: string) {
  return {
    id: nodeCode,
    node_code: nodeCode,
    name: nodeCode,
    agent_version: "0.1.0",
    stored_status: "online" as const,
    effective_status: "online" as const,
    last_heartbeat_at: null,
    created_at: null,
    updated_at: null,
    enabled_task_count: 1,
    disabled_task_count: 0,
    open_alert_count: 0,
    latest_task_run: null,
  };
}

function task(id: string, nodeCode: string) {
  return {
    id,
    node_code: nodeCode,
    data_source_id: "1",
    name: `task-${id}`,
    task_type: "local_file_import",
    schedule_expr: "* * * * *",
    parser_type: "csv_observation",
    qc_profile: "default",
    qc_rule_ids: [],
    enabled: true,
    created_at: null,
    updated_at: null,
  };
}

describe("management pages", () => {
  it("节点详情旧请求失败不会覆盖新请求状态", async () => {
    vi.spyOn(managementApi, "listNodes").mockResolvedValue(emptyPage);
    const first = deferred<ReturnType<typeof nodeSummary>>();
    const second = deferred<ReturnType<typeof nodeSummary>>();
    vi.spyOn(managementApi, "getNode").mockImplementation((nodeCode) =>
      nodeCode === "node-a" ? first.promise : second.promise,
    );
    const router = createRouter({
      history: createMemoryHistory(),
      routes: [{ path: "/nodes", component: NodesView }],
    });
    await router.push("/nodes");
    await router.isReady();
    const wrapper = mount(NodesView, {
      global: {
        plugins: [router],
        stubs: {
          ElAlert: true,
          ElButton: true,
          ElOption: true,
          ElSelect: true,
          ElTable: true,
          ElTableColumn: true,
          LoadMore: true,
          StatusTag: true,
          UtcTime: true,
        },
        directives: { loading: () => undefined },
      },
    });
    await flushPromises();
    const view = wrapper.vm as unknown as {
      selectNode(nodeCode: string): Promise<void>;
      detailLoading: boolean;
      detailError: unknown;
      selectedNode: ReturnType<typeof nodeSummary> | null;
    };

    const oldRequest = view.selectNode("node-a");
    const newRequest = view.selectNode("node-b");
    first.reject(new Error("canceled"));
    await oldRequest;

    expect(view.detailError).toBeNull();
    expect(view.detailLoading).toBe(true);

    second.resolve(nodeSummary("node-b"));
    await newRequest;
    expect(view.selectedNode?.node_code).toBe("node-b");
    expect(view.detailLoading).toBe(false);
  });

  it("节点快速切换时旧任务响应不会覆盖当前任务选项", async () => {
    vi.spyOn(managementApi, "listNodes").mockResolvedValue({
      items: [nodeSummary("node-a"), nodeSummary("node-b")],
      next_cursor: null,
      has_more: false,
    });
    const first = deferred<{
      items: ReturnType<typeof task>[];
      next_cursor: null;
      has_more: false;
    }>();
    const second = deferred<{
      items: ReturnType<typeof task>[];
      next_cursor: null;
      has_more: false;
    }>();
    vi.spyOn(managementApi, "listTasks").mockImplementation((input) =>
      input.node_code === "node-a" ? first.promise : second.promise,
    );
    vi.spyOn(managementApi, "listTaskRuns").mockResolvedValue(emptyPage);
    const router = await routerAt("/task-runs?node_code=node-a");
    const wrapper = mount(TaskRunsView, {
      global: {
        plugins: [router],
        stubs: {
          ElButton: true,
          ElEmpty: true,
          ElOption: true,
          ElSelect: true,
          ElTable: true,
          ElTableColumn: true,
          RunEvidenceDrawer: true,
        },
        directives: { loading: () => undefined },
      },
    });
    await flushPromises();
    const view = wrapper.vm as unknown as {
      nodeCode: string;
      changeNode(): Promise<void>;
      taskOptions: ReturnType<typeof task>[];
      bootstrapLoading: boolean;
      bootstrapError: unknown;
    };

    view.nodeCode = "node-b";
    const change = view.changeNode();
    second.resolve({
      items: [task("task-b", "node-b")],
      next_cursor: null,
      has_more: false,
    });
    await change;
    first.resolve({
      items: [task("task-a", "node-a")],
      next_cursor: null,
      has_more: false,
    });
    await flushPromises();

    expect(view.taskOptions.map((item) => item.node_code)).toEqual(["node-b"]);
    expect(view.bootstrapLoading).toBe(false);
    expect(view.bootstrapError).toBeNull();
  });

  it("没有节点时不发送缺少 node_code 的任务和运行请求", async () => {
    vi.spyOn(managementApi, "listNodes").mockResolvedValue(emptyPage);
    const listTasks = vi
      .spyOn(managementApi, "listTasks")
      .mockResolvedValue(emptyPage);
    const listRuns = vi
      .spyOn(managementApi, "listTaskRuns")
      .mockResolvedValue(emptyPage);

    for (const path of ["/tasks", "/task-runs"]) {
      const router = await routerAt(path);
      const wrapper = mount(
        { template: "<router-view />" },
        {
          global: {
            plugins: [router],
            stubs: {
              ElButton: true,
              ElEmpty: true,
              ElOption: true,
              ElSelect: true,
              ElTable: true,
              ElTableColumn: true,
              RunEvidenceDrawer: true,
            },
            directives: { loading: () => undefined },
          },
        },
      );
      await flushPromises();
      wrapper.unmount();
    }

    expect(listTasks).not.toHaveBeenCalled();
    expect(listRuns).not.toHaveBeenCalled();
  });

  it("证据请求部分失败时保留详情和其余三类结果", async () => {
    vi.spyOn(managementApi, "getTaskRun").mockResolvedValue({
      id: "51",
      task_id: "41",
      node_code: "demo-node-001",
      status: "succeeded",
      started_at: null,
      finished_at: null,
      scheduled_for: null,
      trigger_type: "schedule",
      execution_key: null,
      items_total: 2,
      items_success: 2,
      items_failed: 0,
      error_summary: null,
      stale: false,
      stale_after_seconds: 3600,
      raw_file_count: 1,
      parsed_record_count: 2,
      qc_result_count: 4,
      alert_count: 1,
    });
    vi.spyOn(managementApi, "listRawFiles").mockRejectedValue(
      new Error("原始文件暂时不可用"),
    );
    vi.spyOn(managementApi, "listParsedRecords").mockResolvedValue(emptyPage);
    vi.spyOn(managementApi, "listQcResults").mockResolvedValue(emptyPage);
    vi.spyOn(managementApi, "listAlerts").mockResolvedValue(emptyPage);

    const wrapper = mount(RunEvidenceDrawer, {
      props: { modelValue: true, nodeCode: "demo-node-001", runId: "51" },
      global: {
        stubs: {
          ElAlert: { template: "<div><slot /></div>" },
          ElButton: true,
          ElDrawer: { template: "<div><slot /></div>" },
          ElTable: true,
          ElTableColumn: true,
          ElTabPane: { template: "<section><slot /></section>" },
          ElTabs: { template: "<div><slot /></div>" },
          ElTag: true,
        },
        directives: { loading: () => undefined },
      },
    });
    await flushPromises();

    expect(managementApi.getTaskRun).toHaveBeenCalledWith(
      "51",
      "demo-node-001",
      expect.objectContaining({ signal: expect.any(AbortSignal) }),
    );
    expect(managementApi.listParsedRecords).toHaveBeenCalledOnce();
    expect(managementApi.listQcResults).toHaveBeenCalledOnce();
    expect(managementApi.listAlerts).toHaveBeenCalledOnce();
    expect(wrapper.findAllComponents(ErrorNotice)).toHaveLength(1);
    expect(wrapper.text()).toContain("2/2");
  });
});
