<script setup lang="ts">
import { computed } from 'vue'

import type { ApiError, ApiErrorKind } from '@/api/http'

// 分类文案帮助用户区分"网络不通/服务出错/参数不对"，配合重试给出下一步动作
const CATEGORY_TEXT: Record<ApiErrorKind, string> = {
  network: '网络连接失败，请检查网络或后端服务是否可用',
  timeout: '请求超时，请稍后重试',
  not_found: '请求的资源不存在',
  conflict: '操作与当前状态冲突',
  bad_request: '请求参数不被服务端接受',
  server: '服务端处理出错',
}

const props = defineProps<{ error: ApiError | null }>()
const emit = defineEmits<{ retry: [] }>()

const categoryText = computed(() =>
  props.error != null ? CATEGORY_TEXT[props.error.kind] : '',
)
</script>

<template>
  <div v-if="error != null" class="error-banner" role="alert">
    <div class="error-banner__summary">
      <span class="error-banner__title">{{ categoryText }}</span>
      <el-button size="small" @click="emit('retry')">重试</el-button>
    </div>
    <div v-if="error.message" class="error-banner__detail">{{ error.message }}</div>
  </div>
</template>

<style scoped>
.error-banner {
  padding: 8px 12px;
  border: 1px solid #fbc4c4;
  background: #fef0f0;
  color: #c45656;
  border-radius: 4px;
}

.error-banner__summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.error-banner__title {
  font-weight: 600;
}

.error-banner__detail {
  margin-top: 4px;
  word-break: break-all;
}
</style>
