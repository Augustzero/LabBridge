<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from "vue";
import { useRouter } from "vue-router";

import { managementApi, type NodeListInput } from "@/api/management";
import type { NodeRecord, NodeStatus, NodeSummary } from "@/api/types";
import ErrorNotice from "@/components/ErrorNotice.vue";
import LoadMore from "@/components/LoadMore.vue";
import StatusTag from "@/components/StatusTag.vue";
import UtcTime from "@/components/UtcTime.vue";
import { useKeysetPage } from "@/composables/useKeysetPage";

const router = useRouter();
const status = ref<NodeStatus | "">("");
const selectedNode = ref<NodeSummary | null>(null);
const detailError = ref<unknown>(null);
const detailNodeCode = ref("");
const detailLoading = ref(false);
let detailController: AbortController | null = null;
let detailGeneration = 0;

const nodes = useKeysetPage((filter: NodeListInput, cursor, signal) =>
  managementApi.listNodes({ ...filter, limit: 20, cursor }, { signal }),
);

function currentFilter(): NodeListInput {
  return status.value ? { status: status.value } : {};
}

function applyFilter() {
  detailGeneration += 1;
  detailController?.abort();
  selectedNode.value = null;
  detailNodeCode.value = "";
  detailError.value = null;
  detailLoading.value = false;
  void nodes.loadFirst(currentFilter());
}

async function selectNode(nodeCode: string) {
  const generation = ++detailGeneration;
  detailNodeCode.value = nodeCode;
  detailController?.abort();
  const controller = new AbortController();
  detailController = controller;
  detailLoading.value = true;
  detailError.value = null;
  try {
    const node = await managementApi.getNode(nodeCode, {
      signal: controller.signal,
    });
    if (generation === detailGeneration && !controller.signal.aborted) {
      selectedNode.value = node;
    }
  } catch (error: unknown) {
    if (generation === detailGeneration && !controller.signal.aborted) {
      detailError.value = error;
    }
  } finally {
    if (generation === detailGeneration && !controller.signal.aborted) {
      detailLoading.value = false;
    }
  }
}

function handleRowClick(row: NodeRecord) {
  void selectNode(row.node_code);
}

function goTo(path: "/tasks" | "/task-runs") {
  if (selectedNode.value) {
    void router.push({
      path,
      query: { node_code: selectedNode.value.node_code },
    });
  }
}

onMounted(applyFilter);
onBeforeUnmount(() => {
  nodes.cancel();
  detailGeneration += 1;
  detailController?.abort();
});
</script>

<template>
  <section class="page-card">
    <div class="page-heading">
      <div>
        <p class="page-eyebrow">CONTROL PLANE</p>
        <h2>节点</h2>
        <p>查看 Agent 在线状态、最近心跳和节点业务摘要。</p>
      </div>
      <el-button :loading="nodes.loading.value" @click="applyFilter"
        >刷新</el-button
      >
    </div>

    <div class="filter-bar">
      <el-select
        v-model="status"
        aria-label="在线状态"
        placeholder="全部状态"
        clearable
        @change="applyFilter"
      >
        <el-option label="在线" value="online" />
        <el-option label="离线" value="offline" />
      </el-select>
    </div>

    <ErrorNotice
      v-if="nodes.error.value"
      :error="nodes.error.value"
      @retry="applyFilter"
    />
    <el-table
      v-else
      :data="nodes.items.value"
      v-loading="nodes.loading.value"
      empty-text="没有符合条件的节点"
      @row-click="handleRowClick"
    >
      <el-table-column prop="node_code" label="节点代码" min-width="160" />
      <el-table-column prop="name" label="名称" min-width="150" />
      <el-table-column
        prop="agent_version"
        label="Agent 版本"
        min-width="120"
      />
      <el-table-column label="状态" width="100"
        ><template #default="{ row }"
          ><StatusTag :value="row.effective_status" /></template
      ></el-table-column>
      <el-table-column label="最近心跳" min-width="190"
        ><template #default="{ row }"
          ><UtcTime :value="row.last_heartbeat_at" /></template
      ></el-table-column>
      <el-table-column label="操作" width="100"
        ><template #default="{ row }"
          ><el-button
            link
            type="primary"
            @click.stop="selectNode(row.node_code)"
            >查看摘要</el-button
          ></template
        ></el-table-column
      >
    </el-table>
    <LoadMore
      :loading="nodes.loading.value"
      :has-more="nodes.hasMore.value"
      :item-count="nodes.items.value.length"
      @load="nodes.loadMore"
    />

    <ErrorNotice
      v-if="detailError"
      class="detail-panel"
      :error="detailError"
      @retry="selectNode(detailNodeCode)"
    />
    <section v-if="selectedNode" v-loading="detailLoading" class="detail-panel">
      <div class="detail-title">
        <div>
          <p class="page-eyebrow">NODE SUMMARY</p>
          <h3>{{ selectedNode.name }}</h3>
          <code>{{ selectedNode.node_code }}</code>
        </div>
        <StatusTag :value="selectedNode.effective_status" />
      </div>
      <div class="summary-grid">
        <div>
          <span>已启用任务</span
          ><strong>{{ selectedNode.enabled_task_count }}</strong>
        </div>
        <div>
          <span>已禁用任务</span
          ><strong>{{ selectedNode.disabled_task_count }}</strong>
        </div>
        <div>
          <span>待处理告警</span
          ><strong>{{ selectedNode.open_alert_count }}</strong>
        </div>
        <div>
          <span>最近运行</span
          ><strong>{{ selectedNode.latest_task_run?.id ?? "—" }}</strong>
        </div>
      </div>
      <div v-if="selectedNode.latest_task_run" class="latest-run">
        <StatusTag :value="selectedNode.latest_task_run.status" />
        <span
          >成功 {{ selectedNode.latest_task_run.items_success }}/{{
            selectedNode.latest_task_run.items_total
          }}</span
        >
        <UtcTime :value="selectedNode.latest_task_run.finished_at" />
      </div>
      <div class="detail-actions">
        <el-button type="primary" @click="goTo('/tasks')"
          >查看该节点任务</el-button
        ><el-button @click="goTo('/task-runs')">查看运行历史</el-button>
      </div>
    </section>
  </section>
</template>
