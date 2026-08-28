<script setup lang="ts">
import { ref, watch } from "vue";

import { managementApi } from "@/api/management";
import type {
  AlertRecord,
  ParsedRecord,
  QcResultRecord,
  RawFileRecord,
  TaskRunSummary,
} from "@/api/types";
import { useKeysetPage } from "@/composables/useKeysetPage";
import ErrorNotice from "./ErrorNotice.vue";
import LoadMore from "./LoadMore.vue";
import StatusTag from "./StatusTag.vue";
import UtcTime from "./UtcTime.vue";

const props = defineProps<{
  modelValue: boolean;
  nodeCode: string;
  runId: string | null;
}>();
const emit = defineEmits<{ "update:modelValue": [value: boolean] }>();

const detail = ref<TaskRunSummary | null>(null);
const detailError = ref<unknown>(null);
const detailLoading = ref(false);
let detailGeneration = 0;
let detailController: AbortController | null = null;

const rawFiles = useKeysetPage<RawFileRecord, string>((runId, cursor, signal) =>
  managementApi.listRawFiles(
    { task_run_id: runId, limit: 20, cursor },
    { signal },
  ),
);
const parsedRecords = useKeysetPage<ParsedRecord, string>(
  (runId, cursor, signal) =>
    managementApi.listParsedRecords(
      { task_run_id: runId, limit: 20, cursor },
      { signal },
    ),
);
const qcResults = useKeysetPage<QcResultRecord, string>(
  (runId, cursor, signal) =>
    managementApi.listQcResults(
      { task_run_id: runId, limit: 20, cursor },
      { signal },
    ),
);
const alerts = useKeysetPage<AlertRecord, { runId: string; nodeCode: string }>(
  (filter, cursor, signal) =>
    managementApi.listAlerts(
      {
        task_run_id: filter.runId,
        node_code: filter.nodeCode,
        limit: 20,
        cursor,
      },
      { signal },
    ),
);

function jsonPreview(payload: Record<string, unknown>) {
  return JSON.stringify(payload, null, 2);
}

async function loadDetail() {
  if (!props.runId || !props.nodeCode) return;
  detailGeneration += 1;
  const generation = detailGeneration;
  detailController?.abort();
  detailController = new AbortController();
  detail.value = null;
  detailError.value = null;
  detailLoading.value = true;

  try {
    const result = await managementApi.getTaskRun(props.runId, props.nodeCode, {
      signal: detailController.signal,
    });
    if (generation === detailGeneration) detail.value = result;
  } catch (error: unknown) {
    if (generation === detailGeneration && !detailController.signal.aborted) {
      detailError.value = error;
    }
  } finally {
    if (generation === detailGeneration) detailLoading.value = false;
  }
}

function loadEvidence() {
  if (!props.runId || !props.nodeCode) return;
  // 四类证据各自维护请求状态，一类失败时其余 tab 仍然可以继续查看。
  void rawFiles.loadFirst(props.runId);
  void parsedRecords.loadFirst(props.runId);
  void qcResults.loadFirst(props.runId);
  void alerts.loadFirst({ runId: props.runId, nodeCode: props.nodeCode });
}

function close() {
  emit("update:modelValue", false);
}

function cancelRequests() {
  detailGeneration += 1;
  detailController?.abort();
  rawFiles.cancel();
  parsedRecords.cancel();
  qcResults.cancel();
  alerts.cancel();
}

watch(
  () => [props.modelValue, props.nodeCode, props.runId] as const,
  ([visible]) => {
    if (visible && props.runId && props.nodeCode) {
      void loadDetail();
      loadEvidence();
    } else {
      // 抽屉关闭后不再保留请求，避免旧 run 的响应混进下一次打开。
      cancelRequests();
    }
  },
  { immediate: true },
);
</script>

<template>
  <el-drawer
    :model-value="modelValue"
    size="min(960px, 94vw)"
    title="运行证据"
    destroy-on-close
    @close="close"
  >
    <div v-loading="detailLoading" class="evidence-drawer">
      <ErrorNotice
        v-if="detailError"
        :error="detailError"
        @retry="loadDetail"
      />

      <template v-if="detail">
        <div class="summary-grid">
          <div>
            <span>运行 ID</span><strong>{{ detail.id }}</strong>
          </div>
          <div>
            <span>状态</span
            ><StatusTag :value="detail.status" :stale="detail.stale" />
          </div>
          <div>
            <span>记录</span
            ><strong
              >{{ detail.items_success }}/{{ detail.items_total }}</strong
            >
          </div>
          <div><span>开始时间</span><UtcTime :value="detail.started_at" /></div>
        </div>
        <div class="evidence-counts">
          <el-tag>原始文件 {{ detail.raw_file_count }}</el-tag>
          <el-tag>解析记录 {{ detail.parsed_record_count }}</el-tag>
          <el-tag>质控结果 {{ detail.qc_result_count }}</el-tag>
          <el-tag>告警 {{ detail.alert_count }}</el-tag>
        </div>
      </template>

      <el-tabs class="evidence-tabs">
        <el-tab-pane label="原始文件">
          <ErrorNotice
            v-if="rawFiles.error.value"
            :error="rawFiles.error.value"
            compact
            @retry="rawFiles.loadFirst(runId!)"
          />
          <el-table
            v-else
            :data="rawFiles.items.value"
            v-loading="rawFiles.loading.value"
            empty-text="没有原始文件"
          >
            <el-table-column
              prop="original_name"
              label="文件名"
              min-width="160"
            />
            <el-table-column
              prop="ingest_status"
              label="接入状态"
              width="110"
            />
            <el-table-column prop="size_bytes" label="字节数" width="100" />
            <el-table-column
              prop="file_hash"
              label="SHA-256"
              min-width="220"
              show-overflow-tooltip
            />
            <el-table-column
              prop="storage_path"
              label="归档路径"
              min-width="240"
              show-overflow-tooltip
            />
          </el-table>
          <LoadMore
            :loading="rawFiles.loading.value"
            :has-more="rawFiles.hasMore.value"
            :item-count="rawFiles.items.value.length"
            @load="rawFiles.loadMore"
          />
        </el-tab-pane>

        <el-tab-pane label="解析记录">
          <ErrorNotice
            v-if="parsedRecords.error.value"
            :error="parsedRecords.error.value"
            compact
            @retry="parsedRecords.loadFirst(runId!)"
          />
          <el-table
            v-else
            :data="parsedRecords.items.value"
            v-loading="parsedRecords.loading.value"
            empty-text="没有解析记录"
          >
            <el-table-column prop="id" label="记录 ID" min-width="100" />
            <el-table-column prop="station_code" label="站点" min-width="120" />
            <el-table-column prop="device_code" label="设备" min-width="120" />
            <el-table-column
              prop="record_time"
              label="记录时间"
              min-width="180"
            />
            <el-table-column label="Payload" min-width="280">
              <template #default="{ row }">
                <pre class="json-preview">{{ jsonPreview(row.payload) }}</pre>
              </template>
            </el-table-column>
          </el-table>
          <LoadMore
            :loading="parsedRecords.loading.value"
            :has-more="parsedRecords.hasMore.value"
            :item-count="parsedRecords.items.value.length"
            @load="parsedRecords.loadMore"
          />
        </el-tab-pane>

        <el-tab-pane label="质控结果">
          <ErrorNotice
            v-if="qcResults.error.value"
            :error="qcResults.error.value"
            compact
            @retry="qcResults.loadFirst(runId!)"
          />
          <el-table
            v-else
            :data="qcResults.items.value"
            v-loading="qcResults.loading.value"
            empty-text="没有质控结果"
          >
            <el-table-column
              prop="parsed_record_id"
              label="记录 ID"
              min-width="110"
            />
            <el-table-column
              prop="qc_rule_id"
              label="规则 ID"
              min-width="100"
            />
            <el-table-column label="结果" width="100"
              ><template #default="{ row }"
                ><StatusTag :value="row.result" /></template
            ></el-table-column>
            <el-table-column prop="level" label="级别" width="100" />
            <el-table-column prop="message" label="说明" min-width="260" />
          </el-table>
          <LoadMore
            :loading="qcResults.loading.value"
            :has-more="qcResults.hasMore.value"
            :item-count="qcResults.items.value.length"
            @load="qcResults.loadMore"
          />
        </el-tab-pane>

        <el-tab-pane label="告警">
          <ErrorNotice
            v-if="alerts.error.value"
            :error="alerts.error.value"
            compact
            @retry="alerts.loadFirst({ runId: runId!, nodeCode })"
          />
          <el-table
            v-else
            :data="alerts.items.value"
            v-loading="alerts.loading.value"
            empty-text="没有告警"
          >
            <el-table-column prop="alert_type" label="类型" min-width="130" />
            <el-table-column prop="severity" label="严重度" width="100" />
            <el-table-column label="状态" width="100"
              ><template #default="{ row }"
                ><StatusTag :value="row.status" /></template
            ></el-table-column>
            <el-table-column prop="message" label="说明" min-width="300" />
          </el-table>
          <LoadMore
            :loading="alerts.loading.value"
            :has-more="alerts.hasMore.value"
            :item-count="alerts.items.value.length"
            @load="alerts.loadMore"
          />
        </el-tab-pane>
      </el-tabs>
    </div>
  </el-drawer>
</template>
