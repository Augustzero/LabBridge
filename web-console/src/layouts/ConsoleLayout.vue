<script setup lang="ts">
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import { useAuth } from '@/composables/useAuth'

const route = useRoute()
const router = useRouter()
const { signOut } = useAuth()

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

function clearCredentials(): void {
  // 清凭据并回输入页；业务视图随导航整体卸载，已展示数据一并清空
  signOut()
  void router.replace({ name: 'auth' })
}
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
      <el-header height="56px" class="console-header">
        <span>{{ pageTitle }}</span>
        <el-button size="small" @click="clearCredentials">清除访问凭据</el-button>
      </el-header>
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
  justify-content: space-between;
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
