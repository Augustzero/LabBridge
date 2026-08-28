import { nextTick } from 'vue'

import type { Page } from '@/api/types'
import { useKeysetPage, type PageLoader } from './useKeysetPage'

interface Row {
  id: string
  name: string
}

interface Filter {
  status: string
}

function page(items: Row[], next: string | null, more: boolean): Page<Row> {
  return { items, next_cursor: next, has_more: more }
}

describe('useKeysetPage', () => {
  it('按服务端 cursor 加载更多并按 ID 防重复', async () => {
    const loader = vi.fn<PageLoader<Row, Filter>>()
      .mockResolvedValueOnce(page([{ id: '1', name: 'first' }], 'cursor-1', true))
      .mockResolvedValueOnce(page([
        { id: '1', name: 'duplicate' },
        { id: '2', name: 'second' },
      ], null, false))
    const state = useKeysetPage(loader)

    await state.loadFirst({ status: 'online' })
    await state.loadMore()

    expect(loader.mock.calls[0]?.[1]).toBeUndefined()
    expect(loader.mock.calls[1]?.[1]).toBe('cursor-1')
    expect(state.items.value.map((item) => item.id)).toEqual(['1', '2'])
    expect(state.canLoadMore.value).toBe(false)
  })

  it('筛选重置后丢弃迟到的旧响应', async () => {
    let resolveOld: ((value: Page<Row>) => void) | undefined
    const loader: PageLoader<Row, Filter> = (filter) => {
      if (filter.status === 'online') {
        return new Promise((resolve) => {
          resolveOld = resolve
        })
      }
      return Promise.resolve(page([{ id: '2', name: 'offline node' }], null, false))
    }
    const state = useKeysetPage(loader)

    const oldRequest = state.loadFirst({ status: 'online' })
    await nextTick()
    await state.loadFirst({ status: 'offline' })
    resolveOld?.(page([{ id: '1', name: 'late online node' }], null, false))
    await oldRequest

    expect(state.items.value).toEqual([{ id: '2', name: 'offline node' }])
  })

  it('失败时保留错误并允许重新加载', async () => {
    const loader = vi.fn<PageLoader<Row, Filter>>()
      .mockRejectedValueOnce(new Error('temporary'))
      .mockResolvedValueOnce(page([{ id: '3', name: 'recovered' }], null, false))
    const state = useKeysetPage(loader)

    await state.loadFirst({ status: 'online' })
    expect(state.error.value).toBeInstanceOf(Error)

    await state.loadFirst({ status: 'online' })
    expect(state.error.value).toBeNull()
    expect(state.items.value[0]?.id).toBe('3')
  })
})
