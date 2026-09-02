<script setup lang="ts">
import { onMounted, ref, watch } from 'vue'
import { useRoute, useRouter, type LocationQuery } from 'vue-router'

import { listNodes } from '@/api/management'
import type { Node } from '@/api/types'
import ErrorBanner from '@/components/ErrorBanner.vue'
import LoadMoreButton from '@/components/LoadMoreButton.vue'
import StatusBadge from '@/components/StatusBadge.vue'
import UtcTime from '@/components/UtcTime.vue'
import { usePagedList } from '@/composables/usePagedList'

type StatusFilter = '' | 'online' | 'offline'

const route = useRoute()
const router = useRouter()

// 状态筛选写入 URL（status=online|offline），非法值视为"全部"
function readStatusFilter(query: LocationQuery): StatusFilter {
  const value = query.status
  return value === 'online' || value === 'offline' ? value : ''
}

const statusFilter = ref<StatusFilter>(readStatusFilter(route.query))

const {
  items: nodes,
  loading,
  loadingMore,
  error,
  hasMore,
  refresh,
  loadMore,
} = usePagedList<Node>((page, signal) =>
  listNodes(
    {
      status: statusFilter.value || undefined,
      limit: page.limit,
      cursor: page.cursor,
    },
    signal,
  ),
)

// 外部导航（后退/前进/手改 URL）还原筛选；本页 router.replace 触发的
// 变化因值与当前筛选一致，不会二次刷新
watch(
  () => route.query.status,
  () => {
    if (route.name !== 'nodes') {
      return
    }
    const next = readStatusFilter(route.query)
    if (next !== statusFilter.value) {
      statusFilter.value = next
      void refresh()
    }
  },
)

onMounted(() => {
  void refresh()
})

function applyStatusFilter(value: string | number | boolean | undefined): void {
  statusFilter.value = value === 'online' || value === 'offline' ? value : ''
  void router.replace({ query: statusFilter.value ? { status: statusFilter.value } : {} })
  void refresh()
}

function goDetail(row: Node): void {
  void router.push(`/nodes/${encodeURIComponent(row.node_code)}`)
}
</script>

<template>
  <div class="nodes-view">
    <div class="nodes-view__toolbar">
      <el-radio-group
        :model-value="statusFilter"
        @change="applyStatusFilter"
      >
        <el-radio-button value="">全部</el-radio-button>
        <el-radio-button value="online">在线</el-radio-button>
        <el-radio-button value="offline">离线</el-radio-button>
      </el-radio-group>
    </div>

    <ErrorBanner :error="error" @retry="refresh" />

    <el-table
      v-loading="loading"
      :data="nodes"
      class="nodes-view__table"
      empty-text="暂无节点"
      @row-click="goDetail"
    >
      <el-table-column prop="node_code" label="节点编码" min-width="140" />
      <el-table-column prop="name" label="名称" min-width="160" />
      <el-table-column label="状态" width="100">
        <template #default="{ row }">
          <StatusBadge group="node" :value="row.effective_status" />
        </template>
      </el-table-column>
      <el-table-column prop="agent_version" label="Agent 版本" width="120" />
      <el-table-column label="最近心跳" min-width="180">
        <template #default="{ row }">
          <UtcTime :value="row.last_heartbeat_at" />
        </template>
      </el-table-column>
    </el-table>

    <LoadMoreButton
      :loading="loading"
      :loading-more="loadingMore"
      :has-more="hasMore"
      @click="loadMore"
    />
  </div>
</template>

<style scoped>
.nodes-view__toolbar {
  margin-bottom: 12px;
}

.nodes-view__table {
  width: 100%;
  cursor: pointer;
}
</style>
