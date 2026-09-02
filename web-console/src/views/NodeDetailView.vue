<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import { ApiError } from '@/api/http'
import { findNode, listDataSources } from '@/api/management'
import type { DataSource, NodeSummary } from '@/api/types'
import ErrorBanner from '@/components/ErrorBanner.vue'
import StatusBadge from '@/components/StatusBadge.vue'
import UtcTime from '@/components/UtcTime.vue'

const route = useRoute()
const router = useRouter()

const nodeCode = computed(() => String(route.params.nodeCode ?? ''))

// 节点摘要为单次查询，不走分页状态机
const node = ref<NodeSummary | null>(null)
const nodeLoading = ref(false)
const nodeError = ref<ApiError | null>(null)

// 数据源在小型部署中数量有限，与 NodeSelect 一致逐页取全
const dataSources = ref<DataSource[]>([])
const sourcesLoading = ref(false)
const sourcesError = ref<ApiError | null>(null)

async function loadNode(): Promise<void> {
  nodeLoading.value = true
  nodeError.value = null
  try {
    node.value = await findNode(nodeCode.value)
  } catch (err) {
    if (err instanceof Error && err.name === 'CanceledError') {
      return
    }
    nodeError.value = err as ApiError
  } finally {
    nodeLoading.value = false
  }
}

async function loadSources(): Promise<void> {
  sourcesLoading.value = true
  sourcesError.value = null
  try {
    const collected: DataSource[] = []
    let cursor: string | null = null
    do {
      const page = await listDataSources(
        { nodeCode: nodeCode.value, limit: 100, cursor: cursor ?? undefined },
      )
      collected.push(...page.items)
      cursor = page.next_cursor
    } while (cursor !== null)
    dataSources.value = collected
  } catch (err) {
    if (err instanceof Error && err.name === 'CanceledError') {
      return
    }
    sourcesError.value = err as ApiError
  } finally {
    sourcesLoading.value = false
  }
}

function reload(): void {
  void loadNode()
  void loadSources()
}

// 详情之间直接跳转时（如手改 URL）参数变化需要重新加载
watch(nodeCode, () => {
  if (route.name === 'node-detail') {
    reload()
  }
})

onMounted(reload)

// 跳转参数取自已加载的节点数据，避免导航后 route 参数变化导致丢失
function goTasks(): void {
  if (node.value != null) {
    void router.push({ path: '/tasks', query: { node: node.value.node_code } })
  }
}

function goRuns(): void {
  if (node.value != null) {
    void router.push({ path: '/runs', query: { node: node.value.node_code } })
  }
}

function goBack(): void {
  void router.push('/nodes')
}
</script>

<template>
  <div v-loading="nodeLoading" class="node-detail">
    <div class="node-detail__actions">
      <el-button size="small" @click="goBack">返回节点列表</el-button>
    </div>

    <ErrorBanner :error="nodeError" @retry="loadNode" />

    <template v-if="node != null">
      <el-descriptions title="节点信息" :column="3" border class="node-detail__section">
        <el-descriptions-item label="节点编码">{{ node.node_code }}</el-descriptions-item>
        <el-descriptions-item label="名称">{{ node.name }}</el-descriptions-item>
        <el-descriptions-item label="状态">
          <StatusBadge group="node" :value="node.effective_status" />
        </el-descriptions-item>
        <el-descriptions-item label="Agent 版本">{{ node.agent_version }}</el-descriptions-item>
        <el-descriptions-item label="最近心跳">
          <UtcTime :value="node.last_heartbeat_at" />
        </el-descriptions-item>
        <el-descriptions-item label="创建时间">
          <UtcTime :value="node.created_at" />
        </el-descriptions-item>
      </el-descriptions>

      <div class="node-detail__stats">
        <el-card shadow="never" class="node-detail__stat">
          <div class="node-detail__stat-value">{{ node.enabled_task_count }}</div>
          <div class="node-detail__stat-label">启用任务</div>
        </el-card>
        <el-card shadow="never" class="node-detail__stat">
          <div class="node-detail__stat-value">{{ node.disabled_task_count }}</div>
          <div class="node-detail__stat-label">停用任务</div>
        </el-card>
        <el-card shadow="never" class="node-detail__stat">
          <div class="node-detail__stat-value">{{ node.open_alert_count }}</div>
          <div class="node-detail__stat-label">未处理告警</div>
        </el-card>
      </div>

      <el-descriptions title="最近一次运行" :column="3" border class="node-detail__section">
        <template v-if="node.latest_task_run != null">
          <el-descriptions-item label="运行 ID">{{ node.latest_task_run.id }}</el-descriptions-item>
          <el-descriptions-item label="状态">
            <StatusBadge group="run" :value="node.latest_task_run.status" />
          </el-descriptions-item>
          <el-descriptions-item label="开始时间">
            <UtcTime :value="node.latest_task_run.started_at" />
          </el-descriptions-item>
        </template>
        <template v-else>
          <el-descriptions-item label="运行 ID">暂无运行记录</el-descriptions-item>
        </template>
      </el-descriptions>

      <div class="node-detail__links">
        <el-button size="small" @click="goTasks">查看该节点任务</el-button>
        <el-button size="small" @click="goRuns">查看该节点运行</el-button>
      </div>

      <ErrorBanner :error="sourcesError" @retry="loadSources" />

      <el-collapse class="node-detail__section">
        <el-collapse-item :title="`数据源（${dataSources.length}）`" name="sources">
          <el-table
            v-loading="sourcesLoading"
            :data="dataSources"
            empty-text="该节点暂无数据源"
          >
            <el-table-column prop="source_type" label="类型" width="160" />
            <el-table-column prop="name" label="名称" min-width="180" />
            <el-table-column label="启用状态" width="120">
              <template #default="{ row }">
                <StatusBadge group="task" :value="row.enabled" />
              </template>
            </el-table-column>
          </el-table>
        </el-collapse-item>
      </el-collapse>
    </template>
  </div>
</template>

<style scoped>
.node-detail__actions {
  margin-bottom: 12px;
}

.node-detail__section {
  margin-bottom: 16px;
}

.node-detail__stats {
  display: flex;
  gap: 12px;
  margin-bottom: 16px;
}

.node-detail__stat {
  width: 160px;
}

.node-detail__stat-value {
  font-size: 24px;
  font-weight: 600;
}

.node-detail__stat-label {
  margin-top: 4px;
  color: #909399;
  font-size: 13px;
}

.node-detail__links {
  margin-bottom: 16px;
}
</style>
