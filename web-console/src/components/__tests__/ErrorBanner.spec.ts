import { mount } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { describe, expect, it } from 'vitest'

import { ApiError } from '@/api/http'

import ErrorBanner from '../ErrorBanner.vue'

function mountBanner(error: ApiError | null) {
  return mount(ErrorBanner, {
    props: { error },
    global: { plugins: [ElementPlus] },
  })
}

describe('ErrorBanner', () => {
  it('error 为 null 时不渲染', () => {
    const wrapper = mountBanner(null)

    expect(wrapper.find('.error-banner').exists()).toBe(false)
  })

  it.each([
    { kind: 'network', text: '网络连接失败' },
    { kind: 'timeout', text: '请求超时' },
    { kind: 'not_found', text: '资源不存在' },
    { kind: 'conflict', text: '冲突' },
    { kind: 'bad_request', text: '请求参数' },
    { kind: 'server', text: '服务端处理出错' },
  ] as const)('$kind 显示对应分类文案', ({ kind, text }) => {
    const wrapper = mountBanner(new ApiError(kind, 'backend detail'))

    expect(wrapper.find('.error-banner__title').text()).toContain(text)
    expect(wrapper.find('.error-banner__detail').text()).toBe('backend detail')
  })

  it('点击重试按钮发出 retry 事件', async () => {
    const wrapper = mountBanner(new ApiError('network', 'connection refused'))

    await wrapper.find('button').trigger('click')

    expect(wrapper.emitted('retry')).toHaveLength(1)
  })

  it('服务端 message 为空时不渲染详情行', () => {
    const wrapper = mountBanner(new ApiError('server', ''))

    expect(wrapper.find('.error-banner__detail').exists()).toBe(false)
  })
})
