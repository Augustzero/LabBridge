import { computed, ref } from 'vue'

import { ApiError, type ApiErrorKind } from '@/api/http'
import type { ManualTaskRunAcceptance } from '@/api/types'
import { newIdempotencyKey } from '@/utils/idempotency'

export type ManualRunSubmitFn = (
  idempotencyKey: string,
) => Promise<ManualTaskRunAcceptance>

// 「执行一次」和「重试失败文件」共用的人工提交状态机：
// - 正常路径：生成新幂等键提交，拿到确定结果（202 或明确报错）后本次操作结束。
// - 结果不确定（超时、断网、5xx 或无法识别的响应）：服务端可能已受理，
//   保留原键交给界面确认；用户确认重试时原样复用，已受理的话服务端会
//   返回同一运行，不会重复执行。
// - 明确业务拒绝（参数错误、无权限、不存在、冲突）：操作直接结束，
//   下次点击用新键重新来。
// 刷新页面后状态自然丢弃，不自动补发；操作员按提示去运行历史确认结果。
export function useManualRunSubmit(submit: ManualRunSubmitFn) {
  const submitting = ref(false)
  const pendingKey = ref<string | null>(null)
  const error = ref<ApiError | null>(null)

  // 非空表示有一笔未确认的提交在等用户决定
  const awaitingRetry = computed(() => pendingKey.value !== null)

  // 这三类错误说明“受理没受理说不清”：server 覆盖 5xx 和无法识别的
  // 响应格式，都不能当成“服务端没收到”。
  function isUndetermined(kind: ApiErrorKind): boolean {
    return kind === 'timeout' || kind === 'network' || kind === 'server'
  }

  async function start(): Promise<ManualTaskRunAcceptance | null> {
    if (submitting.value) {
      return null
    }
    error.value = null
    submitting.value = true
    // 幂等键就是这一次操作的请求编号：重试同一操作必须复用一个键，
    // 换新键会让服务端把同一操作再执行一遍。
    const key = pendingKey.value ?? newIdempotencyKey()
    pendingKey.value = key
    try {
      const acceptance = await submit(key)
      pendingKey.value = null
      return acceptance
    } catch (err) {
      if (err instanceof ApiError && isUndetermined(err.kind)) {
        // 保留原键走“重试提交 / 放弃”提示，不弹错误横幅
        return null
      }
      pendingKey.value = null
      if (err instanceof Error && err.name === 'CanceledError') {
        return null
      }
      error.value = err as ApiError
      return null
    } finally {
      submitting.value = false
    }
  }

  function abandon(): void {
    pendingKey.value = null
    error.value = null
  }

  return { submitting, awaitingRetry, error, start, abandon }
}
