<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from "vue";
import { useRoute, useRouter } from "vue-router";

import { managementApi } from "@/api/management";
import type {
  NodeRecord,
  TaskRecord,
  TaskRunRecord,
  TaskRunStatus,
} from "@/api/types";
import ErrorNotice from "@/components/ErrorNotice.vue";
import LoadMore from "@/components/LoadMore.vue";
import RunEvidenceDrawer from "@/components/RunEvidenceDrawer.vue";
import StatusTag from "@/components/StatusTag.vue";
import UtcTime from "@/components/UtcTime.vue";
import { useKeysetPage } from "@/composables/useKeysetPage";

type RunFilter = { nodeCode: string; taskId?: string; status?: TaskRunStatus };

const route = useRoute();
const router = useRouter();
const nodeCode = ref(
  typeof route.query.node_code === "string" ? route.query.node_code : "",
);
const taskId = ref(
  typeof route.query.task_id === "string" ? route.query.task_id : "",
);
const status = ref<TaskRunStatus | "">(
  ["pending", "running", "succeeded", "failed"].includes(
    String(route.query.status),
  )
    ? (route.query.status as TaskRunStatus)
    : "",
);
const activeRunId = ref<string | null>(
  typeof route.query.run_id === "string" ? route.query.run_id : null,
);
const drawerVisible = ref(activeRunId.value !== null);
const nodeOptions = ref<NodeRecord[]>([]);
const taskOptions = ref<TaskRecord[]>([]);
const bootstrapLoading = ref(false);
const bootstrapError = ref<unknown>(null);
let contextGeneration = 0;
let contextController: AbortController | null = null;

const runs = useKeysetPage<TaskRunRecord, RunFilter>((filter, cursor, signal) =>
  managementApi.listTaskRuns(
    {
      node_code: filter.nodeCode,
      task_id: filter.taskId,
      status: filter.status,
      limit: 20,
      cursor,
    },
    { signal },
  ),
);

function runFilter(): RunFilter {
  const filter: RunFilter = { nodeCode: nodeCode.value };
  if (taskId.value) filter.taskId = taskId.value;
  if (status.value) filter.status = status.value;
  return filter;
}

function queryFor(runId = activeRunId.value) {
  return {
    node_code: nodeCode.value || undefined,
    task_id: taskId.value || undefined,
    status: status.value || undefined,
    run_id: runId || undefined,
  };
}

function loadRuns() {
  if (!nodeCode.value) return;
  void router.replace({ query: queryFor() });
  void runs.loadFirst(runFilter());
}

async function loadTasksForNode(
  targetNodeCode: string,
  generation: number,
  signal: AbortSignal,
) {
  taskOptions.value = [];
  if (!targetNodeCode) return false;
  const page = await managementApi.listTasks(
    { node_code: targetNodeCode, limit: 100 },
    { signal },
  );
  // 切换节点后，旧节点的任务响应不能覆盖当前下拉选项。
  if (generation !== contextGeneration || signal.aborted) return false;
  taskOptions.value = page.items;
  if (taskId.value && !page.items.some((task) => task.id === taskId.value)) {
    taskId.value = "";
  }
  return true;
}

async function bootstrap() {
  const generation = ++contextGeneration;
  contextController?.abort();
  const controller = new AbortController();
  contextController = controller;
  bootstrapLoading.value = true;
  bootstrapError.value = null;
  try {
    const nodesPage = await managementApi.listNodes(
      { limit: 100 },
      { signal: controller.signal },
    );
    if (generation !== contextGeneration || controller.signal.aborted) return;
    nodeOptions.value = nodesPage.items;
    if (!nodeCode.value && nodesPage.items[0]) {
      nodeCode.value = nodesPage.items[0].node_code;
    }
    if (!nodeCode.value) return;
    if (await loadTasksForNode(nodeCode.value, generation, controller.signal)) {
      loadRuns();
    }
  } catch (error: unknown) {
    if (generation === contextGeneration && !controller.signal.aborted) {
      bootstrapError.value = error;
    }
  } finally {
    if (generation === contextGeneration && !controller.signal.aborted) {
      bootstrapLoading.value = false;
    }
  }
}

async function changeNode() {
  const targetNodeCode = nodeCode.value;
  const generation = ++contextGeneration;
  contextController?.abort();
  const controller = new AbortController();
  contextController = controller;
  taskId.value = "";
  activeRunId.value = null;
  drawerVisible.value = false;
  bootstrapLoading.value = true;
  bootstrapError.value = null;
  try {
    if (await loadTasksForNode(targetNodeCode, generation, controller.signal)) {
      loadRuns();
    }
  } catch (error: unknown) {
    if (generation === contextGeneration && !controller.signal.aborted) {
      bootstrapError.value = error;
    }
  } finally {
    if (generation === contextGeneration && !controller.signal.aborted) {
      bootstrapLoading.value = false;
    }
  }
}

function openEvidence(run: TaskRunRecord) {
  activeRunId.value = run.id;
  drawerVisible.value = true;
  void router.replace({ query: queryFor(run.id) });
}

function updateDrawer(visible: boolean) {
  drawerVisible.value = visible;
  if (!visible) {
    activeRunId.value = null;
    void router.replace({ query: queryFor(null) });
  }
}

onMounted(bootstrap);
onBeforeUnmount(() => {
  contextGeneration += 1;
  contextController?.abort();
  runs.cancel();
});
</script>

<template>
  <section class="page-card">
    <div class="page-heading">
      <div>
        <p class="page-eyebrow">EVIDENCE</p>
        <h2>运行历史</h2>
        <p>定位一次执行并查看原始文件、解析记录、质控结果与告警证据。</p>
      </div>
      <el-button
        :disabled="!nodeCode"
        :loading="runs.loading.value"
        @click="loadRuns"
        >刷新</el-button
      >
    </div>

    <ErrorNotice
      v-if="bootstrapError"
      :error="bootstrapError"
      @retry="bootstrap"
    />
    <div v-else class="filter-bar">
      <el-select
        v-model="nodeCode"
        aria-label="节点"
        placeholder="请选择节点"
        filterable
        :loading="bootstrapLoading"
        @change="changeNode"
      >
        <el-option
          v-for="node in nodeOptions"
          :key="node.node_code"
          :label="`${node.name}（${node.node_code}）`"
          :value="node.node_code"
        />
      </el-select>
      <el-select
        v-model="taskId"
        aria-label="任务"
        placeholder="全部任务"
        clearable
        @change="loadRuns"
      >
        <el-option
          v-for="task in taskOptions"
          :key="task.id"
          :label="`${task.name}（${task.id}）`"
          :value="task.id"
        />
      </el-select>
      <el-select
        v-model="status"
        aria-label="运行状态"
        placeholder="全部状态"
        clearable
        @change="loadRuns"
      >
        <el-option label="等待中" value="pending" /><el-option
          label="运行中"
          value="running"
        />
        <el-option label="成功" value="succeeded" /><el-option
          label="失败"
          value="failed"
        />
      </el-select>
    </div>

    <el-empty
      v-if="!bootstrapLoading && !bootstrapError && nodeOptions.length === 0"
      description="当前没有节点，Agent 运行后历史会显示在这里"
    />
    <template v-else-if="nodeCode">
      <ErrorNotice
        v-if="runs.error.value"
        :error="runs.error.value"
        @retry="loadRuns"
      />
      <el-table
        v-else
        :data="runs.items.value"
        v-loading="runs.loading.value"
        empty-text="没有符合条件的运行记录"
      >
        <el-table-column prop="id" label="运行 ID" min-width="100" />
        <el-table-column prop="task_id" label="任务 ID" min-width="100" />
        <el-table-column label="状态" min-width="150"
          ><template #default="{ row }"
            ><StatusTag :value="row.status" :stale="row.stale" /></template
        ></el-table-column>
        <el-table-column prop="trigger_type" label="触发方式" min-width="110" />
        <el-table-column label="计划时间" min-width="190"
          ><template #default="{ row }"
            ><UtcTime :value="row.scheduled_for" /></template
        ></el-table-column>
        <el-table-column label="开始时间" min-width="190"
          ><template #default="{ row }"
            ><UtcTime :value="row.started_at" /></template
        ></el-table-column>
        <el-table-column label="完成时间" min-width="190"
          ><template #default="{ row }"
            ><UtcTime :value="row.finished_at" /></template
        ></el-table-column>
        <el-table-column label="记录（成功/总数/失败）" min-width="180"
          ><template #default="{ row }"
            >{{ row.items_success }}/{{ row.items_total }}/{{
              row.items_failed
            }}</template
          ></el-table-column
        >
        <el-table-column
          prop="error_summary"
          label="错误摘要"
          min-width="220"
          show-overflow-tooltip
          ><template #default="{ row }">{{
            row.error_summary || "—"
          }}</template></el-table-column
        >
        <el-table-column fixed="right" label="操作" width="110"
          ><template #default="{ row }"
            ><el-button link type="primary" @click="openEvidence(row)"
              >查看证据</el-button
            ></template
          ></el-table-column
        >
      </el-table>
      <LoadMore
        :loading="runs.loading.value"
        :has-more="runs.hasMore.value"
        :item-count="runs.items.value.length"
        @load="runs.loadMore"
      />
    </template>

    <RunEvidenceDrawer
      :model-value="drawerVisible"
      :node-code="nodeCode"
      :run-id="activeRunId"
      @update:model-value="updateDrawer"
    />
  </section>
</template>
