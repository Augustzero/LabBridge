import axios, { AxiosError, CanceledError } from 'axios'

import { currentToken, notifyCredentialsRejected } from './credentials'

// 后端无 CORS 且有严格参数白名单，前端只能同源访问；
// 开发期由 Vite proxy、生产由 Nginx 反代 /api，代码中不出现后端绝对地址。
export const httpClient = axios.create({
  baseURL: '/',
})

// 所有业务请求统一带管理凭据；输入页验证走同一入口（先保存再验证），
// 不接受 URL 参数或 body 传 token
httpClient.interceptors.request.use((config) => {
  const token = currentToken()
  if (token != null) {
    config.headers.set('Authorization', `Bearer ${token}`)
  }
  return config
})

export type ApiErrorKind =
  | 'network'
  | 'timeout'
  | 'server'
  | 'unauthenticated'
  | 'forbidden'
  | 'not_found'
  | 'conflict'
  | 'bad_request'

export class ApiError extends Error {
  readonly kind: ApiErrorKind
  readonly status?: number
  readonly code?: string

  constructor(kind: ApiErrorKind, message: string, status?: number, code?: string) {
    super(message)
    this.name = 'ApiError'
    this.kind = kind
    this.status = status
    this.code = code
  }
}

interface ErrorBody {
  code?: string
  message?: string
}

interface Envelope<T> {
  ok?: boolean
  data?: T
  error?: ErrorBody
}

function kindFromStatus(status: number): ApiErrorKind {
  if (status === 401) {
    return 'unauthenticated'
  }
  if (status === 403) {
    return 'forbidden'
  }
  if (status === 404) {
    return 'not_found'
  }
  if (status === 409) {
    return 'conflict'
  }
  if (status === 400 || status === 415) {
    return 'bad_request'
  }
  return 'server'
}

function toApiError(error: unknown): ApiError {
  if (error instanceof AxiosError) {
    if (error.response != null) {
      const body = error.response.data as Envelope<unknown> | undefined
      return new ApiError(
        kindFromStatus(error.response.status),
        body?.error?.message ?? error.message,
        error.response.status,
        body?.error?.code,
      )
    }
    if (error.code === 'ECONNABORTED') {
      return new ApiError('timeout', error.message)
    }
    return new ApiError('network', error.message)
  }
  return new ApiError('server', String(error))
}

// 统一请求入口：解包 {"ok":true,"data":...}，失败抛 ApiError；
// 取消异常原样抛出，由调用方通过 generation 计数忽略，不当作页面错误展示。
export async function request<T>(
  config: Parameters<typeof httpClient.request>[0],
  signal?: AbortSignal,
): Promise<T> {
  // 记下发出请求时的凭据，401 回来时若凭据已经换过，说明是迟到响应
  const tokenAtRequest = currentToken()
  let response: { status: number; data: Envelope<T> }
  try {
    response = await httpClient.request<Envelope<T>>({ ...config, signal })
  } catch (error) {
    if (error instanceof CanceledError) {
      throw error
    }
    const apiError = toApiError(error)
    if (apiError.kind === 'unauthenticated' && tokenAtRequest !== null) {
      // 匿名请求（无凭据）的 401 不广播，避免重复跳转
      notifyCredentialsRejected(tokenAtRequest)
    }
    throw apiError
  }

  const body = response.data
  if (body != null && body.ok === true && body.data !== undefined) {
    return body.data
  }
  if (body != null && body.ok === false && body.error != null) {
    throw new ApiError(
      kindFromStatus(response.status),
      body.error.message ?? 'request failed',
      response.status,
      body.error.code,
    )
  }
  throw new ApiError('server', 'unexpected response envelope', response.status)
}
