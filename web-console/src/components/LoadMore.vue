<script setup lang="ts">
defineProps<{ loading: boolean; hasMore: boolean; itemCount: number }>();
const emit = defineEmits<{ load: [] }>();
</script>

<template>
  <div v-if="itemCount > 0" class="load-more">
    <el-button
      v-if="hasMore"
      :loading="loading"
      :disabled="loading"
      @click="emit('load')"
    >
      加载更多
    </el-button>
    <span v-else class="muted-text">已显示全部 {{ itemCount }} 条</span>
  </div>
</template>
