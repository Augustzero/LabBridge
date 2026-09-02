<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter, type LocationQuery } from 'vue-router'

import { ApiError } from '@/api/http'
import { listTasks, setTaskEnabled } from '@/api/management'
import type { Task } from '@/api/types'
import ErrorBanner from '@/components/ErrorBanner.vue'
import LoadMoreButton from '@/components/LoadMoreButton.vue'
import NodeSelect from '@/components/NodeSelect.vue'
import StatusBadge from '@/components/StatusBadge.vue'
import { usePagedList } from '@/composables/usePagedList'
import { ElMessageBox } from 'element-plus'

type EnabledFilter = '' | 'true' | 'false'

const route = useRoute()
const router = useRouter()

// 服务端要求 node_code 必填，节点是任务页的一等筛选条件
function readNodeCode(query: LocationQuery): string | null {
  const value = query.node
  return typeof value === 'string' && value !== '' ? value : null
}

function readEnabledFilter(query: LocationQuery): EnabledFilter {
  const value = query.enabled
  return value === 'true' || value === 'false' ? value : ''
}

const nodeCode = ref<string | null>(readNodeCode(route.query))
const enabledFilter = ref<EnabledFilter>(readEnabledFilter(route.query))

const {
  items: tasks,
  loading,
  loadingMore,
  error,
  hasMore,
  refresh,
  loadMore,
} = usePagedList<Task>((page, signal) => {
  // 未选节点时服务端会因缺少 node_code 返回 400，直接返回空页由空态文案提示
  if (nodeCode.value === null) {
    return Promise.resolve({ items: [], next_cursor: null, has_more: false })
  }
  return listTasks(
    {
      nodeCode: nodeCode.value,
      enabled:
        enabledFilter.value === '' ? undefined : enabledFilter.value === 'true',
      limit: page.limit,
      cursor: page.cursor,
    },
    signal,
  )
})

const hasNode = computed(() => nodeCode.value !== null)
const emptyText = computed(() =>
  hasNode.value ? '该节点暂无任务' : '请先选择节点',
)

// 外部导航（后退/前进/手改 URL）还原筛选；本页 router.replace 触发的
// 变化因值与当前筛选一致，不会二次刷新
watch(
  () => [route.query.node, route.query.enabled] as const,
  () => {
    if (route.name !== 'tasks') {
      return
    }
    const nextNode = readNodeCode(route.query)
    const nextEnabled = readEnabledFilter(route.query)
    if (nextNode !== nodeCode.value || nextEnabled !== enabledFilter.value) {
      nodeCode.value = nextNode
      enabledFilter.value = nextEnabled
      void refresh()
    }
  },
)

onMounted(() => {
  void refresh()
})

function applyNode(value: string): void {
  nodeCode.value = value
  syncQuery()
  void refresh()
}

function applyEnabledFilter(value: string | number | boolean | undefined): void {
  enabledFilter.value = value === 'true' || value === 'false' ? value : ''
  syncQuery()
  void refresh()
}

function syncQuery(): void {
  const query: Record<string, string> = {}
  if (nodeCode.value !== null) {
    query.node = nodeCode.value
  }
  if (enabledFilter.value !== '') {
    query.enabled = enabledFilter.value
  }
  void router.replace({ query })
}

// 任务启停：开关为受控绑定（不乐观更新），确认并调用成功后才以响应体更新行；
// 服务端拒绝或网络失败时行数据本就未变化，无需回滚逻辑
const togglingId = ref<string | null>(null)
const toggleError = ref<ApiError | null>(null)

async function onToggleEnabled(row: Task): Promise<void> {
  const next = !row.enabled
  const action = next ? '启用' : '停用'
  try {
    await ElMessageBox.confirm(
      `确认${action}任务「${row.name}」？`,
      '任务启停确认',
      { confirmButtonText: action, cancelButtonText: '取消', type: 'warning' },
    )
  } catch {
    // 用户取消确认框，状态不变
    return
  }

  togglingId.value = row.id
  toggleError.value = null
  try {
    const updated = await setTaskEnabled(row.id, next)
    const index = tasks.value.findIndex((task) => task.id === row.id)
    if (index >= 0) {
      tasks.value[index] = updated
    }
  } catch (err) {
    if (err instanceof Error && err.name === 'CanceledError') {
      return
    }
    toggleError.value = err as ApiError
  } finally {
    togglingId.value = null
  }
}
</script>

<template>
  <div class="tasks-view">
    <div class="tasks-view__toolbar">
      <NodeSelect :model-value="nodeCode" @update:model-value="applyNode" />
      <el-radio-group
        class="tasks-view__filter"
        :model-value="enabledFilter"
        :disabled="!hasNode"
        @change="applyEnabledFilter"
      >
        <el-radio-button value="">全部</el-radio-button>
        <el-radio-button value="true">已启用</el-radio-button>
        <el-radio-button value="false">已停用</el-radio-button>
      </el-radio-group>
    </div>

    <ErrorBanner :error="toggleError" />
    <ErrorBanner :error="error" @retry="refresh" />

    <el-table
      v-loading="loading"
      :data="tasks"
      class="tasks-view__table"
      :empty-text="emptyText"
    >
      <el-table-column prop="name" label="任务名称" min-width="160" />
      <el-table-column prop="task_type" label="类型" width="120" />
      <el-table-column prop="schedule_expr" label="调度表达式" min-width="140" />
      <el-table-column prop="parser_type" label="解析器" width="120" />
      <el-table-column prop="qc_profile" label="QC Profile" width="120" />
      <el-table-column label="启用状态" width="100">
        <template #default="{ row }">
          <StatusBadge group="task" :value="row.enabled" />
        </template>
      </el-table-column>
      <el-table-column label="启停" width="90">
        <template #default="{ row }">
          <el-switch
            :model-value="row.enabled"
            :disabled="togglingId !== null"
            @change="onToggleEnabled(row)"
          />
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
.tasks-view__toolbar {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-bottom: 12px;
}

.tasks-view__table {
  width: 100%;
}
</style>
