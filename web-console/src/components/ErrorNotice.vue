<script setup lang="ts">
import { computed } from "vue";

const props = defineProps<{ error: unknown; compact?: boolean }>();
const emit = defineEmits<{ retry: [] }>();

const message = computed(() => {
  if (props.error instanceof Error && props.error.message.trim()) {
    return props.error.message;
  }
  return "加载失败，请稍后重试";
});
</script>

<template>
  <el-alert
    :class="{ 'state-alert--compact': compact }"
    :title="message"
    type="error"
    show-icon
    :closable="false"
  >
    <template #default>
      <el-button link type="primary" @click="emit('retry')">重新加载</el-button>
    </template>
  </el-alert>
</template>
