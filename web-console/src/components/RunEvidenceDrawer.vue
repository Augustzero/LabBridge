<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue'

import { ApiError } from '@/api/http'
import {
  findTaskRun,
  listAlerts,
  listParsedRecords,
  listQcResults,
  listRawFiles,
} from '@/api/management'
import type {
  Alert,
  Page,
  ParsedRecord,
  QcResult,
  RawFile,
  TaskRunDetail,
} from '@/api/types'
import ErrorBanner from '@/components/ErrorBanner.vue'
import LoadMoreButton from '@/components/LoadMoreButton.vue'
import StatusBadge from '@/components/StatusBadge.vue'
import UtcTime from '@/components/UtcTime.vue'
import { usePagedList, type PageRequest } from '@/composables/usePagedList'
import { formatDuration, truncate } from '@/utils/format'

const props = defineProps<{
  runId: string | null
  nodeCode: string | null
}>()

const emit = defineEmits<{ close: [] }>()

const FILE_HASH_MAX = 16
const STORAGE_PATH_MAX = 40

// keyset 分页在 runId/nodeCode 缺失时的空页兜底
function emptyPage<T>(): Page<T> {
  return { items: [], next_cursor: null, has_more: false }
}

function requireRunId(): string | null {
  return props.runId
}

function fetchRawFiles(
  page: PageRequest,
  signal: AbortSignal,
): Promise<Page<RawFile>> {
  const runId = requireRunId()
  if (runId === null) {
    return Promise.resolve(emptyPage())
  }
  return listRawFiles(
    { taskRunId: runId, limit: page.limit, cursor: page.cursor },
    signal,
  )
}

function fetchParsedRecords(
  page: PageRequest,
  signal: AbortSignal,
): Promise<Page<ParsedRecord>> {
  const runId = requireRunId()
  if (runId === null) {
    return Promise.resolve(emptyPage())
  }
  return listParsedRecords(
    { taskRunId: runId, limit: page.limit, cursor: page.cursor },
    signal,
  )
}

function fetchQcResults(
  page: PageRequest,
  signal: AbortSignal,
): Promise<Page<QcResult>> {
  const runId = requireRunId()
  if (runId === null) {
    return Promise.resolve(emptyPage())
  }
  return listQcResults(
    { taskRunId: runId, limit: page.limit, cursor: page.cursor },
    signal,
  )
}

// alerts 端点额外要求 node_code 必填
function fetchAlerts(
  page: PageRequest,
  signal: AbortSignal,
): Promise<Page<Alert>> {
  if (props.runId === null || props.nodeCode === null) {
    return Promise.resolve(emptyPage())
  }
  return listAlerts(
    {
      nodeCode: props.nodeCode,
      taskRunId: props.runId,
      limit: page.limit,
      cursor: page.cursor,
    },
    signal,
  )
}

// reactive 包装：模板内直接访问 items/loading 等无需 .value
const rawFiles = reactive(usePagedList<RawFile>(fetchRawFiles))
const parsedRecords = reactive(usePagedList<ParsedRecord>(fetchParsedRecords))
const qcResults = reactive(usePagedList<QcResult>(fetchQcResults))
const alerts = reactive(usePagedList<Alert>(fetchAlerts))

type EvidenceTab = 'raw-files' | 'parsed-records' | 'qc-results' | 'alerts'

const TAB_LISTS = {
  'raw-files': rawFiles,
  'parsed-records': parsedRecords,
  'qc-results': qcResults,
  alerts,
} as const

// 四个 tab 各自独立分页：打开过的 tab 才发请求（lazy 语义由 loadedTabs 保证）
const activeTab = ref<EvidenceTab>('raw-files')
const loadedTabs = new Set<EvidenceTab>()

function ensureLoaded(tab: EvidenceTab): void {
  if (loadedTabs.has(tab)) {
    return
  }
  loadedTabs.add(tab)
  void TAB_LISTS[tab].refresh()
}

watch(activeTab, (tab) => {
  ensureLoaded(tab)
})

// 运行摘要：单次查询，不走分页状态机
const detail = ref<TaskRunDetail | null>(null)
const detailLoading = ref(false)
const detailError = ref<ApiError | null>(null)
let detailController: AbortController | null = null

async function loadDetail(): Promise<void> {
  if (props.runId === null || props.nodeCode === null) {
    return
  }
  detailController?.abort()
  detailController = new AbortController()
  const signal = detailController.signal
  detailLoading.value = true
  detailError.value = null
  try {
    detail.value = await findTaskRun(props.runId, props.nodeCode, signal)
  } catch (err) {
    if (err instanceof Error && err.name === 'CanceledError') {
      return
    }
    detailError.value = err as ApiError
  } finally {
    // 被取消的请求由关闭/重开流程统一复位状态，不在此覆盖
    if (!signal.aborted) {
      detailLoading.value = false
    }
  }
}

// 关闭即终止四类证据与摘要的全部在途请求并丢弃响应
function closeEvidence(): void {
  loadedTabs.clear()
  for (const list of Object.values(TAB_LISTS)) {
    list.abort()
  }
  detailController?.abort()
  detailController = null
  detail.value = null
  detailError.value = null
  detailLoading.value = false
}

watch(
  () => props.runId,
  (runId) => {
    closeEvidence()
    if (runId === null) {
      return
    }
    activeTab.value = 'raw-files'
    void loadDetail()
    ensureLoaded('raw-files')
  },
  { immediate: true },
)

const title = computed(() =>
  props.runId !== null ? `运行证据 · #${props.runId}` : '运行证据',
)

function formatPayload(payload: Record<string, unknown>): string {
  return JSON.stringify(payload, null, 2)
}
</script>

<template>
  <el-drawer
    class="run-evidence-drawer"
    :model-value="runId !== null"
    :title="title"
    size="720px"
    @close="emit('close')"
  >
    <ErrorBanner :error="detailError" @retry="loadDetail" />

    <div v-loading="detailLoading" class="run-evidence-drawer__summary">
      <el-descriptions v-if="detail" :column="3" border size="small">
        <el-descriptions-item label="运行 ID">#{{ detail.id }}</el-descriptions-item>
        <el-descriptions-item label="状态">
          <StatusBadge group="run" :value="detail.status" />
          <StatusBadge
            v-if="detail.stale"
            group="stale"
            :value="true"
            class="run-evidence-drawer__stale"
          />
        </el-descriptions-item>
        <el-descriptions-item label="触发方式">{{ detail.trigger_type }}</el-descriptions-item>
        <el-descriptions-item label="任务 ID">#{{ detail.task_id }}</el-descriptions-item>
        <el-descriptions-item label="开始时间">
          <UtcTime :value="detail.started_at" />
        </el-descriptions-item>
        <el-descriptions-item label="结束时间">
          <UtcTime :value="detail.finished_at" />
        </el-descriptions-item>
        <el-descriptions-item label="条目（成功/失败）">
          {{ detail.items_success }}/{{ detail.items_failed }}（共 {{ detail.items_total }}）
        </el-descriptions-item>
        <el-descriptions-item label="耗时">
          {{ formatDuration(detail.started_at, detail.finished_at) }}
        </el-descriptions-item>
        <el-descriptions-item label="错误摘要">
          {{ detail.error_summary ?? '—' }}
        </el-descriptions-item>
      </el-descriptions>
    </div>

    <el-tabs v-model="activeTab">
      <el-tab-pane name="raw-files">
        <template #label>
          <el-badge
            :value="detail?.raw_file_count ?? 0"
            :hidden="detail === null"
            type="info"
          >
            原始文件
          </el-badge>
        </template>
        <ErrorBanner :error="rawFiles.error" @retry="rawFiles.refresh" />
        <el-table
          v-loading="rawFiles.loading"
          :data="rawFiles.items"
          empty-text="暂无原始文件"
        >
          <el-table-column prop="original_name" label="文件名" min-width="160" />
          <el-table-column label="文件哈希" min-width="150">
            <template #default="{ row }">
              <el-tooltip :content="row.file_hash" placement="top">
                <span>{{ truncate(row.file_hash, FILE_HASH_MAX) }}</span>
              </el-tooltip>
            </template>
          </el-table-column>
          <el-table-column prop="size_bytes" label="大小（字节）" width="110" />
          <el-table-column prop="ingest_status" label="入库状态" width="100" />
          <el-table-column label="存储路径" min-width="180">
            <template #default="{ row }">
              <el-tooltip :content="row.storage_path" placement="top">
                <span>{{ truncate(row.storage_path, STORAGE_PATH_MAX) }}</span>
              </el-tooltip>
            </template>
          </el-table-column>
        </el-table>
        <LoadMoreButton
          :loading="rawFiles.loading"
          :loading-more="rawFiles.loadingMore"
          :has-more="rawFiles.hasMore"
          @click="rawFiles.loadMore"
        />
      </el-tab-pane>

      <el-tab-pane name="parsed-records">
        <template #label>
          <el-badge
            :value="detail?.parsed_record_count ?? 0"
            :hidden="detail === null"
            type="info"
          >
            解析记录
          </el-badge>
        </template>
        <ErrorBanner :error="parsedRecords.error" @retry="parsedRecords.refresh" />
        <el-table
          v-loading="parsedRecords.loading"
          :data="parsedRecords.items"
          empty-text="暂无解析记录"
        >
          <el-table-column prop="station_code" label="站点" width="110" />
          <el-table-column prop="device_code" label="设备" width="110" />
          <el-table-column label="记录时间" min-width="150">
            <template #default="{ row }">
              <UtcTime :value="row.record_time" />
            </template>
          </el-table-column>
          <el-table-column prop="parse_status" label="解析状态" width="100" />
          <el-table-column label="payload" min-width="260">
            <template #default="{ row }">
              <pre class="run-evidence-drawer__payload">{{ formatPayload(row.payload) }}</pre>
            </template>
          </el-table-column>
        </el-table>
        <LoadMoreButton
          :loading="parsedRecords.loading"
          :loading-more="parsedRecords.loadingMore"
          :has-more="parsedRecords.hasMore"
          @click="parsedRecords.loadMore"
        />
      </el-tab-pane>

      <el-tab-pane name="qc-results">
        <template #label>
          <el-badge
            :value="detail?.qc_result_count ?? 0"
            :hidden="detail === null"
            type="info"
          >
            质控结果
          </el-badge>
        </template>
        <ErrorBanner :error="qcResults.error" @retry="qcResults.refresh" />
        <el-table
          v-loading="qcResults.loading"
          :data="qcResults.items"
          empty-text="暂无质控结果"
        >
          <el-table-column prop="qc_rule_id" label="规则 ID" width="100" />
          <el-table-column prop="level" label="级别" width="90" />
          <el-table-column label="结果" width="100">
            <template #default="{ row }">
              <StatusBadge group="qc" :value="row.result" />
            </template>
          </el-table-column>
          <el-table-column label="消息" min-width="220">
            <template #default="{ row }">
              {{ row.message ?? '—' }}
            </template>
          </el-table-column>
        </el-table>
        <LoadMoreButton
          :loading="qcResults.loading"
          :loading-more="qcResults.loadingMore"
          :has-more="qcResults.hasMore"
          @click="qcResults.loadMore"
        />
      </el-tab-pane>

      <el-tab-pane name="alerts">
        <template #label>
          <el-badge
            :value="detail?.alert_count ?? 0"
            :hidden="detail === null"
            type="info"
          >
            告警
          </el-badge>
        </template>
        <ErrorBanner :error="alerts.error" @retry="alerts.refresh" />
        <el-table
          v-loading="alerts.loading"
          :data="alerts.items"
          empty-text="暂无告警"
        >
          <el-table-column prop="severity" label="级别" width="90" />
          <el-table-column prop="alert_type" label="类型" width="140" />
          <el-table-column prop="message" label="消息" min-width="220" />
          <el-table-column label="状态" width="100">
            <template #default="{ row }">
              <StatusBadge group="alert" :value="row.status" />
            </template>
          </el-table-column>
        </el-table>
        <LoadMoreButton
          :loading="alerts.loading"
          :loading-more="alerts.loadingMore"
          :has-more="alerts.hasMore"
          @click="alerts.loadMore"
        />
      </el-tab-pane>
    </el-tabs>
  </el-drawer>
</template>

<style scoped>
.run-evidence-drawer__summary {
  margin-bottom: 12px;
  min-height: 24px;
}

.run-evidence-drawer__stale {
  margin-left: 6px;
}

.run-evidence-drawer__payload {
  margin: 0;
  white-space: pre-wrap;
  word-break: break-all;
  font-size: 12px;
}
</style>
