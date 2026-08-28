<script setup lang="ts">
import { computed } from "vue";

const props = defineProps<{ value: string | boolean; stale?: boolean }>();
const normalized = computed(() => String(props.value));

const label = computed(() => {
  const labels: Record<string, string> = {
    true: "已启用",
    false: "已禁用",
    online: "在线",
    offline: "离线",
    pending: "等待中",
    running: "运行中",
    succeeded: "成功",
    failed: "失败",
    passed: "通过",
    open: "待处理",
    closed: "已关闭",
  };
  return labels[normalized.value] ?? normalized.value;
});

const type = computed(() => {
  if (props.stale) return "warning";
  if (
    ["true", "online", "succeeded", "passed", "closed"].includes(
      normalized.value,
    )
  )
    return "success";
  if (["false", "offline", "failed"].includes(normalized.value))
    return "danger";
  if (["running", "open"].includes(normalized.value)) return "warning";
  return "info";
});
</script>

<template>
  <span class="status-group">
    <el-tag :type="type" effect="light">{{ label }}</el-tag>
    <el-tag v-if="stale" type="warning" effect="plain">已停滞</el-tag>
  </span>
</template>
