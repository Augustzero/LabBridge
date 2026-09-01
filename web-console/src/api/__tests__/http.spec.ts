import { AxiosError, CanceledError } from 'axios'
import type {
  AxiosAdapter,
  AxiosResponse,
  InternalAxiosRequestConfig,
} from 'axios'
import { beforeEach, describe, expect, it, vi } from 'vitest'

import { ApiError, httpClient, request } from '../http'

interface FakeEnvelopeBody {
  ok?: boolean
  data?: unknown
  error?: { code?: string; message?: string }
}

function fakeAdapter(
  body: FakeEnvelopeBody,
  status = 200,
): AxiosAdapter {
  return vi.fn(async (config: InternalAxiosRequestConfig) => {
    const response: AxiosResponse = {
      data: body,
      status,
      statusText: 'OK',
      headers: {},
      config,
    }
    return response
  }) as unknown as AxiosAdapter
}

function httpErrorAdapter(
  status: number,
  body?: FakeEnvelopeBody,
): AxiosAdapter {
  return vi.fn(async (config: InternalAxiosRequestConfig) => {
    const response: AxiosResponse = {
      data: body,
      status,
      statusText: 'error',
      headers: {},
      config,
    }
    throw new AxiosError('request failed', undefined, config, undefined, response)
  }) as unknown as AxiosAdapter
}

function transportErrorAdapter(code: string): AxiosAdapter {
  return vi.fn(async (config: InternalAxiosRequestConfig) => {
    throw new AxiosError('transport failure', code, config)
  }) as unknown as AxiosAdapter
}

beforeEach(() => {
  httpClient.defaults.adapter = undefined
})

describe('request envelope 解包', () => {
  it('ok=true 时返回 data 字段', async () => {
    httpClient.defaults.adapter = fakeAdapter({
      ok: true,
      data: { items: [1], next_cursor: null, has_more: false },
    })
    await expect(request({ url: '/x' })).resolves.toEqual({
      items: [1],
      next_cursor: null,
      has_more: false,
    })
  })

  it('ok=false 时抛出 ApiError 并携带服务端错误信息', async () => {
    httpClient.defaults.adapter = httpErrorAdapter(404, {
      ok: false,
      error: { code: 'not_found', message: 'node not found' },
    })
    const error = await request({ url: '/x' }).catch((err: unknown) => err)
    expect(error).toBeInstanceOf(ApiError)
    const apiError = error as ApiError
    expect(apiError.kind).toBe('not_found')
    expect(apiError.status).toBe(404)
    expect(apiError.code).toBe('not_found')
    expect(apiError.message).toBe('node not found')
  })

  it('响应不符合 envelope 约定时抛出 server 类错误', async () => {
    httpClient.defaults.adapter = fakeAdapter({ ok: true })
    const error = await request({ url: '/x' }).catch((err: unknown) => err)
    expect(error).toBeInstanceOf(ApiError)
    expect((error as ApiError).kind).toBe('server')
  })
})

describe('错误分类', () => {
  it.each([
    [400, 'bad_request'],
    [415, 'bad_request'],
    [409, 'conflict'],
    [404, 'not_found'],
    [500, 'server'],
    [502, 'server'],
  ] as const)('HTTP %i 归类为 %s', async (status, kind) => {
    httpClient.defaults.adapter = httpErrorAdapter(status, {
      ok: false,
      error: { code: 'x', message: 'boom' },
    })
    const error = (await request({ url: '/x' }).catch((err: unknown) => err)) as ApiError
    expect(error.kind).toBe(kind)
  })

  it('无响应的 Axios 异常归类为 network', async () => {
    httpClient.defaults.adapter = transportErrorAdapter(AxiosError.ERR_NETWORK)
    const error = (await request({ url: '/x' }).catch((err: unknown) => err)) as ApiError
    expect(error.kind).toBe('network')
  })

  it('ECONNABORTED 归类为 timeout', async () => {
    httpClient.defaults.adapter = transportErrorAdapter('ECONNABORTED')
    const error = (await request({ url: '/x' }).catch((err: unknown) => err)) as ApiError
    expect(error.kind).toBe('timeout')
  })
})

describe('请求取消与 signal 透传', () => {
  it('CanceledError 原样抛出，不转换为 ApiError', async () => {
    const canceled = new CanceledError()
    httpClient.defaults.adapter = vi.fn(async () => {
      throw canceled
    }) as unknown as AxiosAdapter
    await expect(request({ url: '/x' })).rejects.toBe(canceled)
    await expect(request({ url: '/x' }).catch((err: unknown) => err)).resolves.toBe(
      canceled,
    )
  })

  it('signal 透传给底层 adapter', async () => {
    const adapter = vi.fn(async (config: InternalAxiosRequestConfig) => {
      const response: AxiosResponse = {
        data: { ok: true, data: null },
        status: 200,
        statusText: 'OK',
        headers: {},
        config,
      }
      return response
    }) as unknown as AxiosAdapter
    httpClient.defaults.adapter = adapter
    const controller = new AbortController()
    await request({ url: '/x' }, controller.signal)
    expect(adapter).toHaveBeenCalled()
    expect(
      (adapter as unknown as ReturnType<typeof vi.fn>).mock.calls[0][0].signal,
    ).toBe(controller.signal)
  })
})
