import { computed, ref } from 'vue'

import { ApiError } from '@/api/http'
import type { ManualTaskRunAcceptance } from '@/api/types'
import { newIdempotencyKey } from '@/utils/idempotency'

export type ManualRunSubmitFn = (
  idempotencyKey: string,
) => Promise<ManualTaskRunAcceptance>

// 「执行一次」和「重试失败文件」共用的人工提交状态机：
// - 正常路径：生成新幂等键提交，拿到确定结果（202 或明确报错）后本次操作结束。
// - 超时路径：响应丢了，服务端可能已受理也可能没有，保留原键交给界面确认；
//   用户确认重试时原样复用，已受理的话服务端会返回同一运行，不会重复执行。
// - 服务端明确拒绝或其他网络错误：操作直接结束，下次点击用新键重新来。
// 刷新页面后状态自然丢弃，不自动补发；操作员按提示去运行历史确认结果。
export function useManualRunSubmit(submit: ManualRunSubmitFn) {
  const submitting = ref(false)
  const pendingKey = ref<string | null>(null)
  const error = ref<ApiError | null>(null)

  // 非空表示有一笔未确认的提交在等用户决定
  const awaitingRetry = computed(() => pendingKey.value !== null)

  async function start(): Promise<ManualTaskRunAcceptance | null> {
    if (submitting.value) {
      return null
    }
    error.value = null
    submitting.value = true
    const key = pendingKey.value ?? newIdempotencyKey()
    pendingKey.value = key
    try {
      const acceptance = await submit(key)
      pendingKey.value = null
      return acceptance
    } catch (err) {
      if (err instanceof ApiError && err.kind === 'timeout') {
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
