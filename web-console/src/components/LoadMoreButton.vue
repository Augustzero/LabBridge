<script setup lang="ts">
// keyset 分页"加载更多"按钮：has_more=false 时整体隐藏，
// 前一页仍在刷新或追加中时禁用（与刷新互斥）
const props = defineProps<{
  loading: boolean
  loadingMore: boolean
  hasMore: boolean
}>()

const emit = defineEmits<{ click: [] }>()
</script>

<template>
  <div v-if="props.hasMore" class="load-more">
    <el-button
      size="small"
      :disabled="props.loading"
      :loading="props.loadingMore"
      @click="emit('click')"
    >
      加载更多
    </el-button>
  </div>
</template>
