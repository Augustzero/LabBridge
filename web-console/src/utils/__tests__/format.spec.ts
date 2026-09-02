import { describe, expect, it } from 'vitest'

import { formatDuration, truncate } from '../format'

describe('formatDuration', () => {
  it('开始或结束时间缺失时显示占位符', () => {
    expect(formatDuration(null, '2026-09-02T08:00:02Z')).toBe('—')
    expect(formatDuration('2026-09-02T08:00:00Z', null)).toBe('—')
    expect(formatDuration(null, null)).toBe('—')
  })

  it('完整起止时间返回秒数', () => {
    expect(
      formatDuration('2026-09-02T08:00:00Z', '2026-09-02T08:00:02Z'),
    ).toBe('2s')
  })

  it('非法时间字符串不产生 NaN 展示', () => {
    expect(formatDuration('not-a-date', '2026-09-02T08:00:02Z')).toBe('—')
  })
})

describe('truncate', () => {
  it('超长文本截断并追加省略号', () => {
    expect(truncate('a'.repeat(61), 60)).toBe(`${'a'.repeat(60)}…`)
  })

  it('未超长文本原样返回', () => {
    expect(truncate('abc', 60)).toBe('abc')
  })
})
