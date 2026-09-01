import { CanceledError } from 'axios'
import { describe, expect, it, vi } from 'vitest'

import { ApiError } from '@/api/http'
import type { Page } from '@/api/types'

import { usePagedList } from '../usePagedList'

interface Deferred<T> {
  promise: Promise<T>
  resolve: (value: T) => void
  reject: (reason?: unknown) => void
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void
  let reject!: (reason?: unknown) => void
  const promise = new Promise<T>((res, rej) => {
    resolve = res
    reject = rej
  })
  return { promise, resolve, reject }
}

function pageOf<T>(
  items: T[],
  hasMore = false,
  nextCursor: string | null = null,
): Page<T> {
  return { items, next_cursor: nextCursor, has_more: hasMore }
}

async function flushMicrotasks(): Promise<void> {
  for (let i = 0; i < 10; i += 1) {
    await Promise.resolve()
  }
}

describe('usePagedList', () => {
  it('refresh：拉取首页并写入状态', async () => {
    const fetcher = vi.fn().mockResolvedValue(pageOf(['a', 'b'], true, 'c1'))
    const state = usePagedList(fetcher)

    await state.refresh()

    expect(fetcher).toHaveBeenCalledWith({}, expect.any(AbortSignal))
    expect(state.items.value).toEqual(['a', 'b'])
    expect(state.hasMore.value).toBe(true)
    expect(state.loading.value).toBe(false)
    expect(state.error.value).toBeNull()
  })

  it('loadMore：携带 cursor 追加，has_more=false 时为空操作', async () => {
    const fetcher = vi
      .fn()
      .mockResolvedValueOnce(pageOf(['a'], true, 'c1'))
      .mockResolvedValueOnce(pageOf(['b', 'c'], false, null))
    const state = usePagedList(fetcher)
    await state.refresh()

    await state.loadMore()

    expect(fetcher).toHaveBeenLastCalledWith(
      { cursor: 'c1' },
      expect.any(AbortSignal),
    )
    expect(state.items.value).toEqual(['a', 'b', 'c'])
    expect(state.hasMore.value).toBe(false)

    await state.loadMore()
    expect(fetcher).toHaveBeenCalledTimes(2)
  })

  it('refresh：新请求会中止上一个未完成请求并丢弃其结果', async () => {
    const first = deferred<Page<string>>()
    const second = deferred<Page<string>>()
    const fetcher = vi
      .fn()
      .mockReturnValueOnce(first.promise)
      .mockReturnValueOnce(second.promise)
    const state = usePagedList(fetcher)

    const firstRefresh = state.refresh()
    const secondRefresh = state.refresh()
    const firstSignal = (fetcher.mock.calls[0][1] as AbortSignal)
    const secondSignal = (fetcher.mock.calls[1][1] as AbortSignal)

    second.resolve(pageOf(['latest']))
    first.resolve(pageOf(['stale']))
    await Promise.all([firstRefresh, secondRefresh])
    await flushMicrotasks()

    expect(firstSignal.aborted).toBe(true)
    expect(secondSignal.aborted).toBe(false)
    expect(state.items.value).toEqual(['latest'])
    expect(state.loading.value).toBe(false)
  })

  it('loadMore：被 refresh 中止后不写入结果', async () => {
    const more = deferred<Page<string>>()
    const fetcher = vi
      .fn()
      .mockResolvedValueOnce(pageOf(['a'], true, 'c1'))
      .mockReturnValueOnce(more.promise)
      .mockResolvedValueOnce(pageOf(['replaced']))
    const state = usePagedList(fetcher)
    await state.refresh()

    const loadMorePromise = state.loadMore()
    const refreshPromise = state.refresh()
    more.resolve(pageOf(['late']))
    await Promise.all([loadMorePromise, refreshPromise])
    await flushMicrotasks()

    expect(state.items.value).toEqual(['replaced'])
    expect(state.hasMore.value).toBe(false)
    expect(state.loadingMore.value).toBe(false)
  })

  it('取消异常不进入错误状态', async () => {
    const fetcher = vi.fn().mockRejectedValue(new CanceledError())
    const state = usePagedList(fetcher)

    await state.refresh()

    expect(state.error.value).toBeNull()
    expect(state.loading.value).toBe(false)
  })

  it('ApiError 进入错误状态且保留分类', async () => {
    const apiError = new ApiError('network', 'connection refused')
    const fetcher = vi.fn().mockRejectedValue(apiError)
    const state = usePagedList(fetcher)

    await state.refresh()

    expect(state.error.value).toBe(apiError)
    expect(state.loading.value).toBe(false)
  })

  it('abort：在途请求被中止、状态复位、迟到的响应被丢弃', async () => {
    const pending = deferred<Page<string>>()
    const fetcher = vi.fn().mockReturnValueOnce(pending.promise)
    const state = usePagedList(fetcher)

    const refreshPromise = state.refresh()
    state.abort()
    pending.resolve(pageOf(['late']))
    await refreshPromise
    await flushMicrotasks()

    expect((fetcher.mock.calls[0][1] as AbortSignal).aborted).toBe(true)
    expect(state.items.value).toEqual([])
    expect(state.loading.value).toBe(false)
    expect(state.hasMore.value).toBe(false)
  })

  it('loadMore：进行中时重复调用为空操作', async () => {
    const more = deferred<Page<string>>()
    const fetcher = vi
      .fn()
      .mockResolvedValueOnce(pageOf(['a'], true, 'c1'))
      .mockReturnValueOnce(more.promise)
    const state = usePagedList(fetcher)
    await state.refresh()

    const loadMorePromise = state.loadMore()
    await state.loadMore()
    more.resolve(pageOf(['b']))
    await loadMorePromise

    expect(fetcher).toHaveBeenCalledTimes(2)
  })
})
