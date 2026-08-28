<script setup lang="ts">
import { RouterLink, RouterView } from 'vue-router'

const navigation = [
  { to: '/nodes', label: '节点' },
  { to: '/tasks', label: '任务' },
  { to: '/task-runs', label: '运行历史' },
]
</script>

<template>
  <div class="app-shell">
    <header class="app-header">
      <div>
        <p class="product-kicker">LABBRIDGE</p>
        <h1>现场数据管理台</h1>
      </div>
      <el-tag type="warning" effect="dark">开发演示 · 无认证</el-tag>
    </header>

    <nav class="main-nav" aria-label="主导航">
      <RouterLink v-for="item in navigation" :key="item.to" :to="item.to">
        {{ item.label }}
      </RouterLink>
    </nav>

    <section class="demo-warning" role="note">
      当前实例仅用于本机演示，请勿暴露到公网或生产网络。
    </section>

    <main class="page-content">
      <RouterView />
    </main>
  </div>
</template>
