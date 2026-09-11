import { computed } from 'vue'
import type { Router } from 'vue-router'

import { listNodes } from '@/api/management'
import {
  currentToken,
  dropToken,
  onCredentialsRejected,
  storeToken,
} from '@/api/credentials'

// 认证状态用模块级单例 + composable 暴露，不引入 Pinia；
// token 实体与持久化在 api/credentials.ts，http 层从那边读，避免循环依赖
export function useAuth() {
  const isAuthenticated = computed(() => currentToken() != null)

  // 先保存再验证：验证请求本身要带上这个候选凭据；
  // 失败立即丢弃，关标签页等意外残留也会被路由守卫和 401 兜底清理
  async function signIn(candidate: string): Promise<void> {
    storeToken(candidate)
    try {
      // 复用现有节点列表接口验证凭据，不新增登录 API
      await listNodes({ limit: 1 })
    } catch (err) {
      dropToken()
      throw err
    }
  }

  function signOut(): void {
    dropToken()
  }

  return { isAuthenticated, signIn, signOut }
}

// 业务请求收到"当前凭据失效"的 401 时清凭据并回输入页；
// 在 main.ts 装配一次，redirect 记下原目标，重新输入后回到原页面
export function bindAuthExpiryRedirect(router: Router): void {
  onCredentialsRejected(() => {
    // 输入页自己的验证请求失败会走这里的广播，凭据由 signIn 的 catch 清理
    if (router.currentRoute.value.name === 'auth') {
      return
    }
    dropToken()
    void router.push({
      name: 'auth',
      query: { redirect: router.currentRoute.value.fullPath },
    })
  })
}
