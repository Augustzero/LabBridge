import { ref } from 'vue'

// 管理 token 只放当前标签页的 sessionStorage：刷新后保留、关标签即丢弃，
// 不写 localStorage，也不进 URL、构建变量或静态资源
const STORAGE_KEY = 'labbridge.management-token'

function readStoredToken(): string | null {
  try {
    return window.sessionStorage.getItem(STORAGE_KEY)
  } catch {
    // 存储被禁用等极端情况下按未登录处理，只保留内存态
    return null
  }
}

// token 的 ref 留在本模块内部，外部一律走 currentToken/storeToken/dropToken，
// 保证 ref 和 sessionStorage 始终同步
const token = ref<string | null>(readStoredToken())

export function currentToken(): string | null {
  return token.value
}

export function storeToken(value: string): void {
  token.value = value
  try {
    window.sessionStorage.setItem(STORAGE_KEY, value)
  } catch {
    // 写不进存储不影响当前标签页使用，只是刷新后要重新输入
  }
}

export function dropToken(): void {
  token.value = null
  try {
    window.sessionStorage.removeItem(STORAGE_KEY)
  } catch {
    // 内存态已清空即可
  }
}

type RejectionListener = () => void

const rejectionListeners = new Set<RejectionListener>()

export function onCredentialsRejected(listener: RejectionListener): void {
  rejectionListeners.add(listener)
}

// 401 的归属判断收在这里：只有"发出请求时用的凭据仍是当前凭据"才广播失效，
// 否则是迟到响应——用户已经换过新凭据，不能被旧的 401 清掉
export function notifyCredentialsRejected(usedToken: string): void {
  if (usedToken !== token.value) {
    return
  }
  for (const listener of rejectionListeners) {
    listener()
  }
}
