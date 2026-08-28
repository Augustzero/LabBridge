<script setup lang="ts">
import { computed } from "vue";

const props = defineProps<{ value: string | null }>();
const display = computed(() => {
  if (props.value === null) return "—";
  const timestamp = new Date(props.value);
  return Number.isNaN(timestamp.getTime())
    ? props.value
    : timestamp.toLocaleString();
});
</script>

<template>
  <time
    v-if="value !== null"
    :datetime="value"
    :title="`服务端 UTC：${value}`"
    >{{ display }}</time
  >
  <span v-else class="muted-text">—</span>
</template>
