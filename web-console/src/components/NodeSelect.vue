<script setup lang="ts">
import { onMounted, ref } from 'vue'

import { ApiError } from '@/api/http'
import { listNodes } from '@/api/management'
import type { Node } from '@/api/types'

// 节点选择器：任务页与运行页顶部复用（服务端要求 node_code 必填，
// 界面以节点为中心）。节点数量在小型部署中有限，逐页取全量供选择。
const props = defineProps<{ modelValue: string | null }>()
const emit = defineEmits<{ 'update:modelValue': [value: string] }>()

const options = ref<Node[]>([])
const loading = ref(false)
const error = ref<ApiError | null>(null)

async function loadOptions(): Promise<void> {
  loading.value = true
  error.value = null
  try {
    const collected: Node[] = []
    let cursor: string | null = null
    do {
      const page = await listNodes({ limit: 100, cursor: cursor ?? undefined })
      collected.push(...page.items)
      cursor = page.next_cursor
    } while (cursor !== null)
    options.value = collected
  } catch (err) {
    // 组件卸载触发的取消不是错误
    if (err instanceof Error && err.name === 'CanceledError') {
      return
    }
    error.value = err as ApiError
  } finally {
    loading.value = false
  }
}

onMounted(loadOptions)

defineExpose({ options, loading, error, loadOptions })
</script>

<template>
  <span class="node-select">
    <el-select
      class="node-select__input"
      :model-value="props.modelValue ?? undefined"
      filterable
      placeholder="请选择节点"
      :loading="loading"
      @update:model-value="(value: string) => emit('update:modelValue', value)"
    >
      <el-option
        v-for="node in options"
        :key="node.node_code"
        :value="node.node_code"
        :label="`${node.name}（${node.node_code}）`"
      />
    </el-select>
    <span v-if="error != null" class="node-select__error">
      节点列表加载失败
      <el-button link type="primary" size="small" @click="loadOptions">重试</el-button>
    </span>
  </span>
</template>

<style scoped>
.node-select {
  display: inline-flex;
  align-items: center;
  gap: 12px;
}

.node-select__input {
  width: 260px;
}

.node-select__error {
  color: #c45656;
  font-size: 13px;
}
</style>
