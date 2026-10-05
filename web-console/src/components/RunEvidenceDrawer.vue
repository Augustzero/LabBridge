<script setup lang="ts">
import { computed, h, reactive, ref, watch } from 'vue'
import { useRouter } from 'vue-router'

import { ApiError } from '@/api/http'
import {
  acknowledgeAlert,
  closeAlert,
  findTaskRun,
  listAlerts,
  listParsedRecords,
  listQcResults,
  listRawFiles,
  retryTaskRun,
} from '@/api/management'
import type {
  Alert,
  ManualTaskRunAcceptance,
  Page,
  ParsedRecord,
  QcResult,
  RawFile,
  TaskRunDetail,
  TaskRunFailedFile,
} from '@/api/types'
import ErrorBanner from '@/components/ErrorBanner.vue'
import LoadMoreButton from '@/components/LoadMoreButton.vue'
import StatusBadge from '@/components/StatusBadge.vue'
import UtcTime from '@/components/UtcTime.vue'
import { useManualRunSubmit } from '@/composables/useManualRunSubmit'
import { usePagedList, type PageRequest } from '@/composables/usePagedList'
import {
  formatDuration,
  formatRetryInputMode,
  formatTriggerType,
  truncate,
} from '@/utils/format'
import { ElMessageBox } from 'element-plus'

const props = defineProps<{
  runId: string | null
  nodeCode: string | null
}>()

const emit = defineEmits<{ close: []; submitted: [] }>()

const router = useRouter()

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

// 失败重试与告警处置的交互状态：必须在下面的 runId watch（immediate）
// 之前声明，closeEvidence 复位时会直接引用它们
const {
  submitting: retrySubmitting,
  awaitingRetry: retryAwaiting,
  error: retryError,
  start: startRetrySubmit,
  abandon: abandonRetrySubmit,
} = useManualRunSubmit((idempotencyKey) =>
  retryTaskRun(props.runId as string, idempotencyKey),
)

// 受理结果留在抽屉里直到关闭/切换运行；等待 Agent 的实际开始时间以列表为准
const acceptedRun = ref<ManualTaskRunAcceptance | null>(null)

// 请求期间禁用按钮；成功后直接用响应体里的最新告警刷新行，不整页重置分页
const alertActionId = ref<string | null>(null)
const alertActionError = ref<ApiError | null>(null)

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
  // 人工提交的遗留状态一并复位：换一个运行后旧键/旧受理结果都不能带过去
  abandonRetrySubmit()
  acceptedRun.value = null
  alertActionId.value = null
  alertActionError.value = null
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

// ---------------------------------------------------------------------------
// 失败文件重试
// ---------------------------------------------------------------------------

// 确认框正文：文件清单 + 输入方式 + 固定输入说明。
// 消息框渲染在 body 下，scoped 样式不生效，这里用内联样式保持可读。
function retryConfirmContent(files: TaskRunFailedFile[]) {
  const items = files.map((file) =>
    h('li', { style: 'margin: 2px 0;' }, [
      h('strong', null, `【${formatRetryInputMode(file.archive_raw_file_id)}】`),
      ` ${file.original_name}`,
    ]),
  )
  const nodes = [
    h(
      'p',
      null,
      `将新建关联运行，仅处理以下 ${files.length} 个失败文件，不重新扫描目录：`,
    ),
    h(
      'ul',
      { style: 'max-height: 200px; overflow: auto; margin: 8px 0; padding-left: 18px;' },
      items,
    ),
  ]
  if (files.some((file) => file.archive_raw_file_id !== null)) {
    nodes.push(
      h('p', null, '标注「原归档重放」的文件按原归档内容执行，修改源文件不会改变此次输入。'),
    )
  }
  return h('div', null, nodes)
}

async function onRetryFailedFiles(): Promise<void> {
  const failed = detail.value?.failed_files
  if (!Array.isArray(failed) || failed.length === 0) {
    return
  }
  try {
    await ElMessageBox.confirm(retryConfirmContent(failed), '重试失败文件确认', {
      confirmButtonText: '提交重试',
      cancelButtonText: '取消',
      type: 'warning',
    })
  } catch {
    // 用户取消确认框
    return
  }
  const acceptance = await startRetrySubmit()
  if (acceptance === null) {
    return
  }
  acceptedRun.value = acceptance
  emit('submitted')
}

async function onRetrySubmitPending(): Promise<void> {
  try {
    await ElMessageBox.confirm(
      '上次重试请求未收到响应，服务端可能已受理。是否按原请求重新提交？已受理时会返回同一运行。',
      '重试提交确认',
      { confirmButtonText: '重试提交', cancelButtonText: '取消', type: 'warning' },
    )
  } catch {
    return
  }
  const acceptance = await startRetrySubmit()
  if (acceptance === null) {
    return
  }
  acceptedRun.value = acceptance
  emit('submitted')
}

// 不可重试的原因说明：retryable 只代表终态与清单满足要求，反着讲清楚为什么不能点
const retryBlockedReason = computed<string | null>(() => {
  const current = detail.value
  if (current === null || current.status !== 'failed' || current.retryable) {
    return null
  }
  if (current.failed_file_count === null) {
    return '该运行的报告没有失败文件清单（旧版本报告），不可按文件重试；修复后可对任务执行一次。'
  }
  return '失败未定位到具体文件，不可按文件重试；修复后可对任务执行一次。'
})

// 跳去运行历史看新运行（新运行的 ID 大，默认排在最前），同时关掉抽屉
function viewAcceptedRun(): void {
  const current = detail.value
  if (current === null || acceptedRun.value === null) {
    return
  }
  void router.push({
    path: '/runs',
    query: { node: current.node_code, task: current.task_id },
  })
  emit('close')
}

// ---------------------------------------------------------------------------
// 告警处置
// ---------------------------------------------------------------------------

async function disposeAlert(
  row: Alert,
  action: 'acknowledge' | 'close',
): Promise<void> {
  alertActionId.value = row.id
  alertActionError.value = null
  try {
    const updated =
      action === 'acknowledge'
        ? await acknowledgeAlert(row.id)
        : await closeAlert(row.id)
    const index = alerts.items.findIndex((alert) => alert.id === row.id)
    if (index >= 0) {
      alerts.items[index] = updated
    }
  } catch (err) {
    if (err instanceof Error && err.name === 'CanceledError') {
      return
    }
    alertActionError.value = err as ApiError
  } finally {
    alertActionId.value = null
  }
}

function onAcknowledgeAlert(row: Alert): void {
  void disposeAlert(row, 'acknowledge')
}

async function onCloseAlert(row: Alert): Promise<void> {
  // 关闭是终态动作，多一步确认防止误点
  try {
    await ElMessageBox.confirm(
      '确认关闭该告警？关闭表示本条告警处置结束，原质控结果和运行证据保留。',
      '关闭告警确认',
      { confirmButtonText: '关闭', cancelButtonText: '取消', type: 'warning' },
    )
  } catch {
    return
  }
  await disposeAlert(row, 'close')
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
    <ErrorBanner :error="retryError" @retry="startRetrySubmit" />

    <el-alert
      v-if="retryAwaiting"
      class="run-evidence-drawer__notice"
      type="warning"
      :closable="false"
      title="上次重试请求未收到响应，服务端可能已受理。"
    >
      <div class="run-evidence-drawer__notice-actions">
        <el-button size="small" type="primary" @click="onRetrySubmitPending">
          重试提交
        </el-button>
        <el-button size="small" @click="abandonRetrySubmit">放弃</el-button>
        <span class="run-evidence-drawer__notice-hint">
          放弃后请到运行历史确认结果，不会自动补发
        </span>
      </div>
    </el-alert>

    <el-alert
      v-if="acceptedRun"
      class="run-evidence-drawer__notice"
      type="success"
      :closable="true"
      @close="acceptedRun = null"
    >
      <template #title>
        重试请求已受理：新运行 #{{ acceptedRun.task_run_id
        }}{{ acceptedRun.replayed ? '（重复请求返回同一运行）' : '' }}，等待
        Agent 开始执行。
      </template>
      <div class="run-evidence-drawer__notice-actions">
        <el-button size="small" type="primary" @click="viewAcceptedRun">
          在运行历史中查看
        </el-button>
      </div>
    </el-alert>

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
        <el-descriptions-item label="触发方式">
          {{ formatTriggerType(detail.trigger_type) }}
        </el-descriptions-item>
        <el-descriptions-item label="任务 ID">#{{ detail.task_id }}</el-descriptions-item>
        <el-descriptions-item label="开始时间">
          <UtcTime :value="detail.started_at" />
        </el-descriptions-item>
        <el-descriptions-item label="结束时间">
          <UtcTime :value="detail.finished_at" />
        </el-descriptions-item>
        <el-descriptions-item label="父运行">
          <span v-if="detail.retry_of_run_id !== null" class="run-evidence-drawer__mono">
            #{{ detail.retry_of_run_id }}
          </span>
          <span v-else>—</span>
        </el-descriptions-item>
        <el-descriptions-item label="请求时间">
          <UtcTime :value="detail.requested_at" />
        </el-descriptions-item>
        <el-descriptions-item label="失败文件">
          {{ detail.failed_file_count ?? '—' }}
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

      <div v-if="detail !== null && detail.status === 'failed'" class="run-evidence-drawer__retry">
        <el-button
          size="small"
          type="primary"
          :disabled="!detail.retryable"
          :loading="retrySubmitting"
          @click="onRetryFailedFiles"
        >
          重试失败文件
        </el-button>
        <span v-if="retryBlockedReason !== null" class="run-evidence-drawer__retry-hint">
          {{ retryBlockedReason }}
        </span>
      </div>
    </div>

    <el-collapse
      v-if="detail !== null && detail.failed_files !== null && detail.failed_files.length > 0"
      class="run-evidence-drawer__files"
    >
      <el-collapse-item :title="`失败文件（${detail.failed_files.length}）`" name="failed-files">
        <el-table :data="detail.failed_files" size="small" empty-text="暂无失败文件">
          <el-table-column prop="original_name" label="文件名" min-width="150">
            <template #default="{ row }">
              <el-tooltip :content="row.source_path" placement="top">
                <span>{{ row.original_name }}</span>
              </el-tooltip>
            </template>
          </el-table-column>
          <el-table-column prop="stage" label="失败阶段" width="90" />
          <el-table-column prop="message" label="原因" min-width="160" />
          <el-table-column label="重试输入" width="100">
            <template #default="{ row }">
              {{ formatRetryInputMode(row.archive_raw_file_id) }}
            </template>
          </el-table-column>
        </el-table>
      </el-collapse-item>
    </el-collapse>

    <el-collapse
      v-if="detail !== null && detail.retry_files !== null && detail.retry_files.length > 0"
      class="run-evidence-drawer__files"
    >
      <el-collapse-item :title="`重试输入快照（${detail.retry_files.length}）`" name="retry-files">
        <el-table :data="detail.retry_files" size="small" empty-text="暂无重试输入">
          <el-table-column prop="original_name" label="文件名" min-width="140">
            <template #default="{ row }">
              <el-tooltip :content="row.source_path" placement="top">
                <span>{{ row.original_name }}</span>
              </el-tooltip>
            </template>
          </el-table-column>
          <el-table-column label="输入方式" width="100">
            <template #default="{ row }">
              {{ formatRetryInputMode(row.archive_raw_file_id) }}
            </template>
          </el-table-column>
          <el-table-column prop="size_bytes" label="大小（字节）" width="110" />
          <el-table-column label="内容哈希" min-width="140">
            <template #default="{ row }">
              <el-tooltip :content="row.file_hash" placement="top">
                <span>{{ truncate(row.file_hash, FILE_HASH_MAX) }}</span>
              </el-tooltip>
            </template>
          </el-table-column>
        </el-table>
      </el-collapse-item>
    </el-collapse>

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
        <ErrorBanner :error="alertActionError" />
        <el-table
          v-loading="alerts.loading"
          :data="alerts.items"
          empty-text="暂无告警"
        >
          <el-table-column prop="severity" label="级别" width="80" />
          <el-table-column prop="alert_type" label="类型" width="120" />
          <el-table-column prop="message" label="消息" min-width="170" />
          <el-table-column label="状态" width="90">
            <template #default="{ row }">
              <StatusBadge group="alert" :value="row.status" />
            </template>
          </el-table-column>
          <el-table-column label="处置时间" width="180">
            <template #default="{ row }">
              <div class="run-evidence-drawer__disposed-at">
                <span>确认</span>
                <UtcTime :value="row.acknowledged_at" />
              </div>
              <div class="run-evidence-drawer__disposed-at">
                <span>关闭</span>
                <UtcTime :value="row.closed_at" />
              </div>
            </template>
          </el-table-column>
          <el-table-column label="操作" width="110">
            <template #default="{ row }">
              <template v-if="row.status === 'open'">
                <el-button
                  link
                  type="primary"
                  size="small"
                  :disabled="alertActionId !== null"
                  @click="onAcknowledgeAlert(row)"
                >
                  确认
                </el-button>
                <el-button
                  link
                  type="primary"
                  size="small"
                  :disabled="alertActionId !== null"
                  @click="onCloseAlert(row)"
                >
                  关闭
                </el-button>
              </template>
              <el-button
                v-else-if="row.status === 'acknowledged'"
                link
                type="primary"
                size="small"
                :disabled="alertActionId !== null"
                @click="onCloseAlert(row)"
              >
                关闭
              </el-button>
              <span v-else>—</span>
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

.run-evidence-drawer__notice {
  margin-bottom: 12px;
}

.run-evidence-drawer__notice-actions {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 4px;
}

.run-evidence-drawer__notice-hint {
  color: #909399;
  font-size: 12px;
}

.run-evidence-drawer__retry {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 8px;
}

.run-evidence-drawer__retry-hint {
  color: #909399;
  font-size: 12px;
}

.run-evidence-drawer__files {
  margin-bottom: 12px;
}

.run-evidence-drawer__mono {
  font-family: monospace;
}

.run-evidence-drawer__disposed-at {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 12px;
  color: #606266;
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
