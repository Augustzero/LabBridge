<script setup lang="ts">
import { computed } from 'vue'

// 服务端时间字段为 UTC ISO 8601（YYYY-MM-DDTHH:MM:SSZ）字符串，
// 缺失时为 null（如未上报心跳的节点）
const props = defineProps<{ value: string | null }>()

const EMPTY_TEXT = '—'

const display = computed(() => {
  if (!props.value) {
    return EMPTY_TEXT
  }
  const matched = /^(\d{4}-\d{2}-\d{2})T(\d{2}:\d{2}:\d{2})Z$/.exec(props.value)
  return matched ? `${matched[1]} ${matched[2]} UTC` : props.value
})
</script>

<template>
  <span class="utc-time">{{ display }}</span>
</template>
