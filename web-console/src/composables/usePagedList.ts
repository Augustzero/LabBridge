import { ref, type Ref } from 'vue'

import type { ApiError } from '@/api/http'
import type { Page } from '@/api/types'

export interface PageRequest {
  limit?: number
  cursor?: string
}

export type PageFetcher<T> = (
  page: PageRequest,
  signal: AbortSignal,
) => Promise<Page<T>>

// keyset 分页通用状态机：
// - refresh() 重置 cursor、替换列表；loadMore() 携带 next_cursor 追加。
// - 竞态防护用 generation 计数：响应返回时 generation 不匹配则丢弃结果；
//   新请求发出前 abort 上一个未完成请求，取消异常不进入错误状态。
export function usePagedList<T>(fetcher: PageFetcher<T>) {
  const items: Ref<T[]> = ref([])
  const loading = ref(false)
  const loadingMore = ref(false)
  const error: Ref<ApiError | null> = ref(null)
  const hasMore = ref(false)

  let generation = 0
  let nextCursor: string | null = null
  let controller: AbortController | null = null

  function beginRequest(): { generation: number; signal: AbortSignal } {
    controller?.abort()
    generation += 1
    loading.value = false
    loadingMore.value = false
    error.value = null
    controller = new AbortController()
    return { generation, signal: controller.signal }
  }

  function isCurrent(requestGeneration: number): boolean {
    return requestGeneration === generation
  }

  async function refresh(): Promise<void> {
    const { generation: current, signal } = beginRequest()
    loading.value = true
    try {
      const page = await fetcher({}, signal)
      if (!isCurrent(current)) {
        return
      }
      nextCursor = page.next_cursor
      hasMore.value = page.has_more
      items.value = page.items
    } catch (err) {
      if (isCanceled(err) || !isCurrent(current)) {
        return
      }
      error.value = err as ApiError
    } finally {
      if (isCurrent(current)) {
        loading.value = false
      }
    }
  }

  async function loadMore(): Promise<void> {
    if (loading.value || loadingMore.value || !hasMore.value || nextCursor === null) {
      return
    }
    const cursor = nextCursor
    const previous = items.value
    const { generation: current, signal } = beginRequest()
    loadingMore.value = true
    try {
      const page = await fetcher({ cursor }, signal)
      if (!isCurrent(current)) {
        return
      }
      nextCursor = page.next_cursor
      hasMore.value = page.has_more
      items.value = previous.concat(page.items)
    } catch (err) {
      if (isCanceled(err) || !isCurrent(current)) {
        return
      }
      error.value = err as ApiError
    } finally {
      if (isCurrent(current)) {
        loadingMore.value = false
      }
    }
  }

  // 页面卸载或抽屉关闭时调用：中止在途请求并丢弃其响应
  function abort(): void {
    controller?.abort()
    controller = null
    generation += 1
    loading.value = false
    loadingMore.value = false
  }

  return {
    items,
    loading,
    loadingMore,
    error,
    hasMore,
    refresh,
    loadMore,
    abort,
  }
}

// CanceledError 表示请求被取消而非服务端错误，不应进入错误展示
function isCanceled(err: unknown): boolean {
  return err instanceof Error && err.name === 'CanceledError'
}
