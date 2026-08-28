<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from "vue";
import { useRoute, useRouter } from "vue-router";

import { managementApi } from "@/api/management";
import type { NodeRecord, TaskRecord } from "@/api/types";
import ErrorNotice from "@/components/ErrorNotice.vue";
import LoadMore from "@/components/LoadMore.vue";
import StatusTag from "@/components/StatusTag.vue";
import { useKeysetPage } from "@/composables/useKeysetPage";

type TaskFilter = { nodeCode: string; enabled?: boolean };

const route = useRoute();
const router = useRouter();
const nodeCode = ref(
  typeof route.query.node_code === "string" ? route.query.node_code : "",
);
const enabled = ref<"all" | "true" | "false">("all");
const nodeOptions = ref<NodeRecord[]>([]);
const nodesLoading = ref(false);
const nodesError = ref<unknown>(null);

const tasks = useKeysetPage<TaskRecord, TaskFilter>((filter, cursor, signal) =>
  managementApi.listTasks(
    {
      node_code: filter.nodeCode,
      enabled: filter.enabled,
      limit: 20,
      cursor,
    },
    { signal },
  ),
);

function taskFilter(): TaskFilter {
  const filter: TaskFilter = { nodeCode: nodeCode.value };
  if (enabled.value !== "all") filter.enabled = enabled.value === "true";
  return filter;
}

function loadTasks() {
  if (!nodeCode.value) return;
  void router.replace({ query: { ...route.query, node_code: nodeCode.value } });
  void tasks.loadFirst(taskFilter());
}

async function loadNodes() {
  nodesLoading.value = true;
  nodesError.value = null;
  try {
    const page = await managementApi.listNodes({ limit: 100 });
    nodeOptions.value = page.items;
    if (!nodeCode.value && page.items[0])
      nodeCode.value = page.items[0].node_code;
    // 没有节点时不调用必填 node_code 的任务接口。
    if (nodeCode.value) loadTasks();
  } catch (error: unknown) {
    nodesError.value = error;
  } finally {
    nodesLoading.value = false;
  }
}

function openRuns(task: TaskRecord) {
  void router.push({
    path: "/task-runs",
    query: { node_code: task.node_code, task_id: task.id },
  });
}

onMounted(loadNodes);
onBeforeUnmount(tasks.cancel);
</script>

<template>
  <section class="page-card">
    <div class="page-heading">
      <div>
        <p class="page-eyebrow">CONFIGURATION</p>
        <h2>任务</h2>
        <p>按节点查看采集任务、解析器与有序质控规则绑定。</p>
      </div>
      <el-button
        :disabled="!nodeCode"
        :loading="tasks.loading.value"
        @click="loadTasks"
        >刷新</el-button
      >
    </div>

    <ErrorNotice v-if="nodesError" :error="nodesError" @retry="loadNodes" />
    <div v-else class="filter-bar">
      <el-select
        v-model="nodeCode"
        aria-label="节点"
        placeholder="请选择节点"
        filterable
        :loading="nodesLoading"
        @change="loadTasks"
      >
        <el-option
          v-for="node in nodeOptions"
          :key="node.node_code"
          :label="`${node.name}（${node.node_code}）`"
          :value="node.node_code"
        />
      </el-select>
      <el-select v-model="enabled" aria-label="启用状态" @change="loadTasks">
        <el-option label="全部任务" value="all" />
        <el-option label="已启用" value="true" />
        <el-option label="已禁用" value="false" />
      </el-select>
    </div>

    <el-empty
      v-if="!nodesLoading && !nodesError && nodeOptions.length === 0"
      description="当前没有节点，Agent 注册后任务会显示在这里"
    />
    <template v-else-if="nodeCode">
      <ErrorNotice
        v-if="tasks.error.value"
        :error="tasks.error.value"
        @retry="loadTasks"
      />
      <el-table
        v-else
        :data="tasks.items.value"
        v-loading="tasks.loading.value"
        empty-text="该节点没有符合条件的任务"
      >
        <el-table-column prop="id" label="任务 ID" min-width="100" />
        <el-table-column prop="name" label="名称" min-width="180" />
        <el-table-column label="状态" width="100"
          ><template #default="{ row }"
            ><StatusTag :value="row.enabled" /></template
        ></el-table-column>
        <el-table-column prop="task_type" label="任务类型" min-width="150" />
        <el-table-column prop="schedule_expr" label="调度" min-width="130" />
        <el-table-column prop="parser_type" label="解析器" min-width="150" />
        <el-table-column
          prop="data_source_id"
          label="数据源 ID"
          min-width="110"
        />
        <el-table-column label="质控规则（顺序）" min-width="220"
          ><template #default="{ row }"
            ><span v-if="row.qc_rule_ids.length">{{
              row.qc_rule_ids.join(" → ")
            }}</span
            ><span v-else class="muted-text">未绑定</span></template
          ></el-table-column
        >
        <el-table-column prop="qc_profile" label="质控配置" min-width="150" />
        <el-table-column fixed="right" label="操作" width="110"
          ><template #default="{ row }"
            ><el-button link type="primary" @click="openRuns(row)"
              >查看运行</el-button
            ></template
          ></el-table-column
        >
      </el-table>
      <LoadMore
        :loading="tasks.loading.value"
        :has-more="tasks.hasMore.value"
        :item-count="tasks.items.value.length"
        @load="tasks.loadMore"
      />
    </template>
  </section>
</template>
