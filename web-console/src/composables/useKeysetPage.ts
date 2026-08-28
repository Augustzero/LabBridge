import { computed, ref, shallowRef } from 'vue'

import type { Page } from '@/api/types'

export type PageLoader<T, Filter> = (
  filter: Filter,
  cursor: string | undefined,
  signal: AbortSignal,
) => Promise<Page<T>>

export function useKeysetPage<T extends { id: string }, Filter>(loader: PageLoader<T, Filter>) {
  const items = shallowRef<T[]>([])
  const nextCursor = ref<string | null>(null)
  const hasMore = ref(false)
  const loading = ref(false)
  const error = shallowRef<unknown>(null)

  let generation = 0
  let activeController: AbortController | null = null
  let currentFilter: Filter | undefined

  const canLoadMore = computed(() => hasMore.value && !loading.value)

  async function requestPage(append: boolean) {
    if (currentFilter === undefined || loading.value) {
      return
    }

    const requestGeneration = generation
    const cursor = append ? (nextCursor.value ?? undefined) : undefined
    const controller = new AbortController()
    activeController = controller
    loading.value = true
    error.value = null

    try {
      const page = await loader(currentFilter, cursor, controller.signal)
      // 筛选切换后，旧请求即使没有响应取消，也不能把新结果顶掉。
      if (requestGeneration !== generation) {
        return
      }

      if (append) {
        const knownIds = new Set(items.value.map((item) => item.id))
        items.value = [
          ...items.value,
          ...page.items.filter((item) => !knownIds.has(item.id)),
        ]
      } else {
        items.value = page.items
      }
      nextCursor.value = page.next_cursor
      hasMore.value = page.has_more
    } catch (requestError: unknown) {
      if (requestGeneration === generation && !controller.signal.aborted) {
        error.value = requestError
      }
    } finally {
      if (requestGeneration === generation) {
        loading.value = false
      }
    }
  }

  async function loadFirst(filter: Filter) {
    generation += 1
    activeController?.abort()
    currentFilter = filter
    items.value = []
    nextCursor.value = null
    hasMore.value = false
    loading.value = false
    await requestPage(false)
  }

  async function loadMore() {
    if (!canLoadMore.value) {
      return
    }
    await requestPage(true)
  }

  function cancel() {
    generation += 1
    activeController?.abort()
    loading.value = false
  }

  return {
    items,
    nextCursor,
    hasMore,
    loading,
    error,
    canLoadMore,
    loadFirst,
    loadMore,
    cancel,
  }
}
