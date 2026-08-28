import axios, { AxiosError, type AxiosInstance } from 'axios'

import { ApiError, NetworkError, ProtocolError } from './errors'

interface SuccessEnvelope {
  ok: true
  data: unknown
}

interface ErrorEnvelope {
  ok: false
  error: {
    code: string
    message: string
  }
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function isSuccessEnvelope(value: unknown): value is SuccessEnvelope {
  return isObject(value)
    && value.ok === true
    && Object.prototype.hasOwnProperty.call(value, 'data')
}

function isErrorEnvelope(value: unknown): value is ErrorEnvelope {
  if (!isObject(value) || value.ok !== false || !isObject(value.error)) {
    return false
  }
  return typeof value.error.code === 'string' && typeof value.error.message === 'string'
}

export function createApiClient(instance?: AxiosInstance) {
  const transport = instance ?? axios.create({
    baseURL: '/api/v1',
    timeout: 10_000,
    headers: { Accept: 'application/json' },
  })

  return {
    async get<T>(path: string, config?: { params?: object; signal?: AbortSignal }): Promise<T> {
      try {
        const response = await transport.get<unknown>(path, config)
        if (!isSuccessEnvelope(response.data)) {
          throw new ProtocolError()
        }
        return response.data.data as T
      } catch (error: unknown) {
        if (error instanceof ProtocolError) {
          throw error
        }
        if (error instanceof AxiosError) {
          if (error.response && isErrorEnvelope(error.response.data)) {
            throw new ApiError(
              error.response.data.error.code,
              error.response.data.error.message,
              error.response.status,
            )
          }
          if (error.response) {
            throw new ProtocolError()
          }
          if (error.code === 'ERR_CANCELED') {
            throw error
          }
          throw new NetworkError(
            error.code === 'ECONNABORTED' ? '连接 LabBridge 服务超时，请重试' : undefined,
          )
        }
        throw error
      }
    },
  }
}

export const apiClient = createApiClient()
