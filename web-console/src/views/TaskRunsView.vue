<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter, type LocationQuery } from 'vue-router'

import { ApiError } from '@/api/http'
import { listTasks, listTaskRuns } from '@/api/management'
import type { Task, TaskRunWithStale } from '@/api/types'
import ErrorBanner from '@/components/ErrorBanner.vue'
import LoadMoreButton from '@/components/LoadMoreButton.vue'
import NodeSelect from '@/components/NodeSelect.vue'
import RunEvidenceDrawer from '@/components/RunEvidenceDrawer.vue'
import StatusBadge from '@/components/StatusBadge.vue'
import UtcTime from '@/components/UtcTime.vue'
import { usePagedList } from '@/composables/usePagedList'
import { formatDuration, truncate } from '@/utils/format'

type StatusFilter = '' | 'pending' | 'running' | 'succeeded' | 'failed'

const STATUS_VALUES: StatusFilter[] = ['pending', 'running', 'succeeded', 'failed']

const route = useRoute()
const router = useRouter()

function readNodeCode(query: LocationQuery): string | null {
  const value = query.node
  return typeof value === 'string' && value !== '' ? value : null
}

function readTaskId(query: LocationQuery): string | null {
  const value = query.task
  return typeof value === 'string' && value !== '' ? value : null
}

function readStatusFilter(query: LocationQuery): StatusFilter {
  const value = query.status
  return typeof value === 'string' && STATUS_VALUES.includes(value as StatusFilter)
    ? (value as StatusFilter)
    : ''
}

const nodeCode = ref<string | null>(readNodeCode(route.query))
const taskIdFilter = ref<string | null>(readTaskId(route.query))
const statusFilter = ref<StatusFilter>(readStatusFilter(route.query))

const {
  items: runs,
  loading,
  loadingMore,
  error,
  hasMore,
  refresh,
  loadMore,
} = usePagedList<TaskRunWithStale>((page, signal) => {
  // 未选节点时服务端会因缺少 node_code 返回 400，直接返回空页由空态文案提示
  if (nodeCode.value === null) {
    return Promise.resolve({ items: [], next_cursor: null, has_more: false })
  }
  return listTaskRuns(
    {
      nodeCode: nodeCode.value,
      taskId: taskIdFilter.value ?? undefined,
      status: statusFilter.value === '' ? undefined : statusFilter.value,
      limit: page.limit,
      cursor: page.cursor,
    },
    signal,
  )
})

const hasNode = computed(() => nodeCode.value !== null)
const emptyText = computed(() =>
  hasNode.value ? '该节点暂无运行记录' : '请先选择节点',
)

// 任务下拉的选项来自该节点任务列表；未选节点时不请求。
// keyset 分页无总数语义，与 NodeSelect 一致按 limit=100 循环取全。
const tasks = ref<Task[]>([])
const tasksLoading = ref(false)
const tasksError = ref<ApiError | null>(null)

async function loadTasks(): Promise<void> {
  if (nodeCode.value === null) {
    tasks.value = []
    return
  }
  tasksLoading.value = true
  tasksError.value = null
  try {
    const collected: Task[] = []
    let cursor: string | null = null
    do {
      const page = await listTasks({
        nodeCode: nodeCode.value,
        limit: 100,
        cursor: cursor ?? undefined,
      })
      collected.push(...page.items)
      cursor = page.next_cursor
    } while (cursor !== null)
    tasks.value = collected
    // 节点切换或任务被移除后，URL 中遗留的 task 筛选可能失效，需清除
    if (
      taskIdFilter.value !== null &&
      !tasks.value.some((task) => task.id === taskIdFilter.value)
    ) {
      taskIdFilter.value = null
      syncQuery()
    }
  } catch (err) {
    if (err instanceof Error && err.name === 'CanceledError') {
      return
    }
    tasksError.value = err as ApiError
  } finally {
    tasksLoading.value = false
  }
}

function onNodeChange(value: string): void {
  nodeCode.value = value
  taskIdFilter.value = null
  syncQuery()
  void refresh()
  void loadTasks()
}

function applyTaskFilter(value: string): void {
  taskIdFilter.value = value === '' ? null : value
  syncQuery()
  void refresh()
}

function applyStatusFilter(value: string | number | boolean | undefined): void {
  statusFilter.value =
    typeof value === 'string' && STATUS_VALUES.includes(value as StatusFilter)
      ? (value as StatusFilter)
      : ''
  syncQuery()
  void refresh()
}

function syncQuery(): void {
  const query: Record<string, string> = {}
  if (nodeCode.value !== null) {
    query.node = nodeCode.value
  }
  if (taskIdFilter.value !== null) {
    query.task = taskIdFilter.value
  }
  if (statusFilter.value !== '') {
    query.status = statusFilter.value
  }
  void router.replace({ query })
}

// 外部导航（后退/前进/手改 URL）还原筛选；本页 router.replace 触发的
// 变化因值与当前筛选一致，不会二次刷新
watch(
  () => [route.query.node, route.query.task, route.query.status] as const,
  () => {
    if (route.name !== 'runs') {
      return
    }
    const nextNode = readNodeCode(route.query)
    const nextTask = readTaskId(route.query)
    const nextStatus = readStatusFilter(route.query)
    if (
      nextNode !== nodeCode.value ||
      nextTask !== taskIdFilter.value ||
      nextStatus !== statusFilter.value
    ) {
      const nodeChanged = nextNode !== nodeCode.value
      nodeCode.value = nextNode
      taskIdFilter.value = nextTask
      statusFilter.value = nextStatus
      if (nodeChanged) {
        void loadTasks()
      }
      void refresh()
    }
  },
)

onMounted(() => {
  void refresh()
  void loadTasks()
})

const ERROR_SUMMARY_MAX = 60

// 证据抽屉：持有当前查看的 run id，行点击打开
const drawerRunId = ref<string | null>(null)

function openDrawer(row: TaskRunWithStale): void {
  drawerRunId.value = row.id
}

function closeDrawer(): void {
  drawerRunId.value = null
}
</script>

<template>
  <div class="task-runs-view">
    <div class="task-runs-view__toolbar">
      <NodeSelect :model-value="nodeCode" @update:model-value="onNodeChange" />
      <el-select
        class="task-runs-view__task-filter"
        :model-value="taskIdFilter ?? ''"
        :disabled="!hasNode"
        :loading="tasksLoading"
        placeholder="全部任务"
        @update:model-value="applyTaskFilter"
      >
        <el-option value="" label="全部任务" />
        <el-option
          v-for="task in tasks"
          :key="task.id"
          :value="task.id"
          :label="task.name"
        />
      </el-select>
      <el-radio-group
        class="task-runs-view__filter"
        :model-value="statusFilter"
        :disabled="!hasNode"
        @change="applyStatusFilter"
      >
        <el-radio-button value="">全部</el-radio-button>
        <el-radio-button value="pending">等待中</el-radio-button>
        <el-radio-button value="running">运行中</el-radio-button>
        <el-radio-button value="succeeded">成功</el-radio-button>
        <el-radio-button value="failed">失败</el-radio-button>
      </el-radio-group>
    </div>

    <ErrorBanner :error="tasksError" />
    <ErrorBanner :error="error" @retry="refresh" />

    <el-table
      v-loading="loading"
      :data="runs"
      class="task-runs-view__table"
      :empty-text="emptyText"
      @row-click="openDrawer"
    >
      <el-table-column label="运行 ID" width="100">
        <template #default="{ row }">
          <span class="task-runs-view__run-id">#{{ row.id }}</span>
        </template>
      </el-table-column>
      <el-table-column label="状态" width="150">
        <template #default="{ row }">
          <StatusBadge group="run" :value="row.status" />
          <StatusBadge
            v-if="row.stale"
            group="stale"
            :value="true"
            class="task-runs-view__stale"
          />
        </template>
      </el-table-column>
      <el-table-column label="开始时间" min-width="150">
        <template #default="{ row }">
          <UtcTime :value="row.started_at" />
        </template>
      </el-table-column>
      <el-table-column label="结束时间" min-width="150">
        <template #default="{ row }">
          <UtcTime :value="row.finished_at" />
        </template>
      </el-table-column>
      <el-table-column label="耗时" width="80">
        <template #default="{ row }">
          {{ formatDuration(row.started_at, row.finished_at) }}
        </template>
      </el-table-column>
      <el-table-column label="条目（成功/失败）" width="150">
        <template #default="{ row }">
          {{ row.items_success }}/{{ row.items_failed }}（共 {{ row.items_total }}）
        </template>
      </el-table-column>
      <el-table-column label="错误摘要" min-width="200">
        <template #default="{ row }">
          <el-tooltip
            v-if="row.error_summary"
            :content="row.error_summary"
            placement="top"
          >
            <span>{{ truncate(row.error_summary, ERROR_SUMMARY_MAX) }}</span>
          </el-tooltip>
          <span v-else>—</span>
        </template>
      </el-table-column>
    </el-table>

    <LoadMoreButton
      :loading="loading"
      :loading-more="loadingMore"
      :has-more="hasMore"
      @click="loadMore"
    />

    <RunEvidenceDrawer
      :run-id="drawerRunId"
      :node-code="nodeCode"
      @close="closeDrawer"
    />
  </div>
</template>

<style scoped>
.task-runs-view__toolbar {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-bottom: 12px;
}

.task-runs-view__task-filter {
  width: 220px;
}

.task-runs-view__table {
  width: 100%;
}

.task-runs-view__run-id {
  font-family: monospace;
}

.task-runs-view__stale {
  margin-left: 6px;
}
</style>
