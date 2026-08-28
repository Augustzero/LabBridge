<script setup lang="ts">
import ManagementLayout from '@/layouts/ManagementLayout.vue'

const unsupportedBrowser = /MSIE|Trident\//.test(window.navigator.userAgent)
</script>

<template>
  <main v-if="unsupportedBrowser" class="browser-block">
    <h1>当前浏览器不受支持</h1>
    <p>请使用新版 Chrome、Edge、Firefox、Safari 或国产浏览器极速模式访问 LabBridge。</p>
  </main>
  <ManagementLayout v-else />
</template>
