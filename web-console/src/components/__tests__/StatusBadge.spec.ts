import { mount } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { describe, expect, it } from 'vitest'

import StatusBadge from '../StatusBadge.vue'

function mountBadge(group: string, value: string | boolean) {
  return mount(StatusBadge, {
    props: { group, value } as never,
    global: { plugins: [ElementPlus] },
  })
}

describe('StatusBadge', () => {
  it.each([
    { group: 'node', value: 'online', colorClass: 'el-tag--success', text: '在线' },
    { group: 'node', value: 'offline', colorClass: 'el-tag--info', text: '离线' },
    { group: 'run', value: 'pending', colorClass: 'el-tag--info', text: '等待中' },
    { group: 'run', value: 'running', colorClass: 'el-tag--primary', text: '运行中' },
    { group: 'run', value: 'succeeded', colorClass: 'el-tag--success', text: '成功' },
    { group: 'run', value: 'failed', colorClass: 'el-tag--danger', text: '失败' },
    { group: 'qc', value: 'passed', colorClass: 'el-tag--success', text: '通过' },
    { group: 'qc', value: 'failed', colorClass: 'el-tag--danger', text: '不通过' },
    { group: 'alert', value: 'open', colorClass: 'el-tag--danger', text: '未处理' },
  ])(
    '$group=$value 渲染 $text 与对应颜色',
    ({ group, value, colorClass, text }) => {
      const wrapper = mountBadge(group, value)

      const tag = wrapper.find('.el-tag')
      expect(tag.classes()).toContain(colorClass)
      expect(tag.text()).toContain(text)
    },
  )

  it('task 启停按布尔值渲染', () => {
    const enabled = mountBadge('task', true)
    expect(enabled.find('.el-tag').classes()).toContain('el-tag--success')
    expect(enabled.find('.el-tag').text()).toContain('已启用')

    const disabled = mountBadge('task', false)
    expect(disabled.find('.el-tag').classes()).toContain('el-tag--info')
    expect(disabled.find('.el-tag').text()).toContain('已停用')
  })

  it('stale=true 渲染橙色已超时，stale=false 不渲染', () => {
    const stale = mountBadge('stale', true)
    expect(stale.find('.el-tag').classes()).toContain('el-tag--warning')
    expect(stale.find('.el-tag').text()).toContain('已超时')

    const notStale = mountBadge('stale', false)
    expect(notStale.find('.el-tag').exists()).toBe(false)
  })

  it('未知字符串枚举值以灰色原始文字兜底', () => {
    const wrapper = mountBadge('alert', 'acknowledged')

    const tag = wrapper.find('.el-tag')
    expect(tag.classes()).toContain('el-tag--info')
    expect(tag.text()).toContain('acknowledged')
  })
})
