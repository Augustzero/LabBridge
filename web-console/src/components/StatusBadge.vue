<script setup lang="ts">
import { computed } from 'vue'

// 状态呈现规范（UI/UX 指南"颜色+文字双通道"）：
// 每个状态由预设色与固定中文文案组成，绝不只靠颜色表达。
type BadgeGroup = 'node' | 'run' | 'qc' | 'alert' | 'task' | 'stale'

interface BadgePreset {
  type: 'success' | 'info' | 'primary' | 'danger' | 'warning'
  text: string
}

const PRESETS: Record<BadgeGroup, Record<string, BadgePreset | undefined>> = {
  node: {
    online: { type: 'success', text: '在线' },
    offline: { type: 'info', text: '离线' },
  },
  run: {
    pending: { type: 'info', text: '等待中' },
    running: { type: 'primary', text: '运行中' },
    succeeded: { type: 'success', text: '成功' },
    failed: { type: 'danger', text: '失败' },
  },
  qc: {
    passed: { type: 'success', text: '通过' },
    failed: { type: 'danger', text: '不通过' },
  },
  alert: {
    open: { type: 'danger', text: '未处理' },
  },
  task: {
    true: { type: 'success', text: '已启用' },
    false: { type: 'info', text: '已停用' },
  },
  // stale=false 不渲染标签，由调用方决定是否展示"已超时"
  stale: {
    true: { type: 'warning', text: '已超时' },
  },
}

const props = defineProps<{
  group: BadgeGroup
  value: string | boolean
}>()

const display = computed<BadgePreset | undefined>(() => {
  const key = typeof props.value === 'boolean' ? String(props.value) : props.value
  return (
    PRESETS[props.group][key] ??
    // 服务端返回未知枚举值时以灰色原始文字兜底（外部数据边界）
    (typeof props.value === 'string'
      ? { type: 'info' as const, text: props.value }
      : undefined)
  )
})
</script>

<template>
  <el-tag v-if="display" :type="display.type" disable-transitions>
    {{ display.text }}
  </el-tag>
</template>
