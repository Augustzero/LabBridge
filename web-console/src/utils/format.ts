// 展示格式化工具：运行页与证据抽屉共用

// 运行耗时：开始或结束时间缺失（等待中/运行中）或非法时显示 "—"
export function formatDuration(
  startedAt: string | null,
  finishedAt: string | null,
): string {
  if (startedAt === null || finishedAt === null) {
    return '—'
  }
  const start = Date.parse(startedAt)
  const end = Date.parse(finishedAt)
  if (Number.isNaN(start) || Number.isNaN(end)) {
    return '—'
  }
  return `${Math.max(0, Math.round((end - start) / 1000))}s`
}

// 长文本在表格/单元格内截断展示，完整内容由 tooltip 呈现
export function truncate(text: string, maxLength: number): string {
  return text.length > maxLength ? `${text.slice(0, maxLength)}…` : text
}

// 触发方式的展示文案；未知值原样展示，服务端新增类型时不至于空白
const TRIGGER_TYPE_TEXTS: Record<string, string> = {
  scheduled: '定时',
  manual: '手动执行',
  retry: '失败重试',
}

export function formatTriggerType(triggerType: string): string {
  return TRIGGER_TYPE_TEXTS[triggerType] ?? triggerType
}

// 失败文件条目的重试输入方式：有归档引用走原归档重放，没有则定点补采。
// 与服务端创建重试运行时的固定规则一致，仅用于展示。
export function formatRetryInputMode(archiveRawFileId: string | null): string {
  return archiveRawFileId !== null ? '原归档重放' : '定点补采'
}
