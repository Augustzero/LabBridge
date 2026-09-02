<script setup lang="ts">
import { computed } from 'vue'
import { useRoute } from 'vue-router'

const route = useRoute()

const NAV_ITEMS = [
  { path: '/nodes', title: '节点管理' },
  { path: '/tasks', title: '任务管理' },
  { path: '/runs', title: '运行历史' },
]

// 节点详情等子路径也高亮所属的导航项
const activePath = computed(
  () => NAV_ITEMS.find((item) => route.path.startsWith(item.path))?.path ?? '',
)
const pageTitle = computed(
  () => (route.meta.title as string | undefined) ?? 'LabBridge 控制台',
)
</script>

<template>
  <el-container class="console-layout">
    <el-aside width="220px" class="console-aside">
      <div class="console-brand">LabBridge 控制台</div>
      <el-menu :default-active="activePath" router class="console-nav">
        <el-menu-item v-for="item in NAV_ITEMS" :key="item.path" :index="item.path">
          {{ item.title }}
        </el-menu-item>
      </el-menu>
    </el-aside>
    <el-container>
      <el-header height="56px" class="console-header">{{ pageTitle }}</el-header>
      <el-main class="console-main">
        <router-view />
      </el-main>
    </el-container>
  </el-container>
</template>

<style scoped>
.console-layout {
  height: 100vh;
}

.console-aside {
  background: #fff;
  border-right: 1px solid var(--labbridge-border);
}

.console-brand {
  height: 56px;
  padding-left: 20px;
  border-bottom: 1px solid var(--labbridge-border);
  font-size: 16px;
  font-weight: 600;
  line-height: 56px;
}

.console-nav {
  border-right: none;
}

.console-header {
  display: flex;
  align-items: center;
  background: #fff;
  border-bottom: 1px solid var(--labbridge-border);
  color: var(--labbridge-text);
  font-size: 15px;
  font-weight: 600;
}

.console-main {
  padding: 16px;
}
</style>
