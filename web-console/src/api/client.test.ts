import axios, { AxiosError, type AxiosAdapter } from 'axios'

import { createApiClient } from './client'
import { ApiError, NetworkError, ProtocolError } from './errors'

function clientWith(adapter: AxiosAdapter) {
  return createApiClient(axios.create({ adapter }))
}

describe('API client', () => {
  it('解包成功响应且不把字符串 ID 转成数字', async () => {
    const client = clientWith(async (config) => ({
      config,
      data: { ok: true, data: { id: '9007199254740993', last_heartbeat_at: null } },
      headers: {},
      status: 200,
      statusText: 'OK',
    }))

    const result = await client.get<{ id: string; last_heartbeat_at: string | null }>(
      '/nodes/demo',
    )

    expect(result).toEqual({ id: '9007199254740993', last_heartbeat_at: null })
    expect(typeof result.id).toBe('string')
  })

  it('映射后端稳定错误', async () => {
    const instance = axios.create()
    instance.get = vi.fn().mockRejectedValue(new AxiosError(
      'bad request',
      'ERR_BAD_REQUEST',
      undefined,
      undefined,
      {
        config: {} as never,
        data: {
          ok: false,
          error: { code: 'invalid_argument', message: 'node_code is required' },
        },
        headers: {},
        status: 400,
        statusText: 'Bad Request',
      },
    ))

    await expect(createApiClient(instance).get('/tasks')).rejects.toEqual(
      new ApiError('invalid_argument', 'node_code is required', 400),
    )
  })

  it('区分网络错误和非法 envelope', async () => {
    const offline = axios.create()
    offline.get = vi.fn().mockRejectedValue(new AxiosError('offline', 'ERR_NETWORK'))
    await expect(createApiClient(offline).get('/nodes')).rejects.toBeInstanceOf(NetworkError)

    const malformed = clientWith(async (config) => ({
      config,
      data: { items: [] },
      headers: {},
      status: 200,
      statusText: 'OK',
    }))
    await expect(malformed.get('/nodes')).rejects.toBeInstanceOf(ProtocolError)

    const malformedFailure = axios.create()
    malformedFailure.get = vi.fn().mockRejectedValue(new AxiosError(
      'bad gateway',
      'ERR_BAD_RESPONSE',
      undefined,
      undefined,
      {
        config: {} as never,
        data: '<html>proxy error</html>',
        headers: {},
        status: 502,
        statusText: 'Bad Gateway',
      },
    ))
    await expect(createApiClient(malformedFailure).get('/nodes'))
      .rejects.toBeInstanceOf(ProtocolError)
  })
})
