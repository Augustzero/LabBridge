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
