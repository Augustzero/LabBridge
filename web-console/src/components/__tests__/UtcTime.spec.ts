import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'

import UtcTime from '../UtcTime.vue'

describe('UtcTime', () => {
  it('null 与空串显示 —', () => {
    expect(mount(UtcTime, { props: { value: null } }).text()).toBe('—')
    expect(mount(UtcTime, { props: { value: '' } }).text()).toBe('—')
  })

  it('UTC ISO 8601 转为可读格式并带 UTC 标记', () => {
    const wrapper = mount(UtcTime, { props: { value: '2026-09-02T01:02:03Z' } })

    expect(wrapper.text()).toBe('2026-09-02 01:02:03 UTC')
  })

  it('非标准格式原样显示', () => {
    const wrapper = mount(UtcTime, { props: { value: '2026-09-02 01:02:03' } })

    expect(wrapper.text()).toBe('2026-09-02 01:02:03')
  })
})
