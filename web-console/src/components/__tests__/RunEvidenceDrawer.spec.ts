import { flushPromises, mount } from '@vue/test-utils'
import ElementPlus from 'element-plus'
import { beforeEach, describe, expect, it, vi } from 'vitest'

import { ApiError } from '@/api/http'
import {
  findTaskRun,
  listAlerts,
  listParsedRecords,
  listQcResults,
  listRawFiles,
} from '@/api/management'
import type {
  Alert,
  Page,
  ParsedRecord,
  QcResult,
  RawFile,
  TaskRunDetail,
} from '@/api/types'

import RunEvidenceDrawer from '../RunEvidenceDrawer.vue'

vi.mock('@/api/management', () => ({
  findTaskRun: vi.fn(),
  listRawFiles: vi.fn(),
  listParsedRecords: vi.fn(),
  listQcResults: vi.fn(),
  listAlerts: vi.fn(),
}))

const findTaskRunMock = vi.mocked(findTaskRun)
const listRawFilesMock = vi.mocked(listRawFiles)
const listParsedRecordsMock = vi.mocked(listParsedRecords)
const listQcResultsMock = vi.mocked(listQcResults)
const listAlertsMock = vi.mocked(listAlerts)

function detailOf(overrides: Partial<TaskRunDetail> = {}): TaskRunDetail {
  return {
    id: '5',
    task_id: '7',
    node_code: 'LAB-01',
    status: 'succeeded',
    started_at: '2026-09-02T08:00:00Z',
    finished_at: '2026-09-02T08:00:02Z',
    scheduled_for: null,
    trigger_type: 'schedule',
    execution_key: null,
    items_total: 2,
    items_success: 2,
    items_failed: 0,
    error_summary: null,
    stale: false,
    stale_after_seconds: 3600,
    raw_file_count: 1,
    parsed_record_count: 2,
    qc_result_count: 4,
    alert_count: 1,
    ...overrides,
  }
}

function rawFileOf(id: string): RawFile {
  return {
    id,
    task_run_id: '5',
    node_code: 'LAB-01',
    original_name: `observations-${id}.csv`,
    file_hash: 'a'.repeat(64),
    storage_path: 'raw/LAB-01/2026/09/02/observations.csv',
    size_bytes: 2048,
    source_mtime: null,
    ingest_status: 'collected',
    created_at: null,
  }
}

function parsedRecordOf(id: string): ParsedRecord {
  return {
    id,
    raw_file_id: '1',
    task_run_id: '5',
    station_code: 'ST-01',
    device_code: 'DEV-01',
    record_time: '2026-09-02T08:00:00Z',
    payload: { temperature: 25.5 },
    parse_status: 'parsed',
    created_at: null,
  }
}

function qcResultOf(id: string, result: 'passed' | 'failed'): QcResult {
  return {
    id,
    task_run_id: '5',
    parsed_record_id: '10',
    qc_rule_id: '2',
    level: 'error',
    result,
    message: result === 'failed' ? '时间戳格式错误' : null,
    created_at: null,
  }
}

function alertOf(id: string): Alert {
  return {
    id,
    node_code: 'LAB-01',
    task_run_id: '5',
    alert_type: 'parse_failed',
    severity: 'error',
    message: '1 条记录解析失败',
    status: 'open',
    created_at: null,
  }
}

function pageOf<T>(items: T[], hasMore = false): Page<T> {
  return { items, next_cursor: null, has_more: hasMore }
}

async function mountDrawer(
  props: { runId: string | null; nodeCode: string | null } = {
    runId: null,
    nodeCode: null,
  },
) {
  const wrapper = mount(RunEvidenceDrawer, {
    props,
    global: { plugins: [ElementPlus] },
  })
  await flushPromises()
  return wrapper
}

async function switchTab(
  wrapper: Awaited<ReturnType<typeof mountDrawer>>,
  tab: string,
): Promise<void> {
  wrapper.findComponent({ name: 'ElTabs' }).vm.$emit('update:modelValue', tab)
  await flushPromises()
}

beforeEach(() => {
  findTaskRunMock.mockReset()
  listRawFilesMock.mockReset()
  listParsedRecordsMock.mockReset()
  listQcResultsMock.mockReset()
  listAlertsMock.mockReset()
  findTaskRunMock.mockResolvedValue(detailOf())
  listRawFilesMock.mockResolvedValue(pageOf([rawFileOf('1')]))
  listParsedRecordsMock.mockResolvedValue(pageOf([parsedRecordOf('10')]))
  listQcResultsMock.mockResolvedValue(pageOf([qcResultOf('3', 'failed')]))
  listAlertsMock.mockResolvedValue(pageOf([alertOf('9')]))
})

describe('RunEvidenceDrawer', () => {
  it('runId 为空时抽屉关闭且不发起任何请求', async () => {
    const wrapper = await mountDrawer()

    expect(wrapper.findComponent({ name: 'ElDrawer' }).props('modelValue')).toBe(
      false,
    )
    expect(findTaskRunMock).not.toHaveBeenCalled()
    expect(listRawFilesMock).not.toHaveBeenCalled()
    expect(listParsedRecordsMock).not.toHaveBeenCalled()
    expect(listQcResultsMock).not.toHaveBeenCalled()
    expect(listAlertsMock).not.toHaveBeenCalled()
  })

  it('打开时加载运行摘要并携带 node_code，默认加载原始文件 tab', async () => {
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await flushPromises()

    expect(findTaskRunMock).toHaveBeenCalledWith('5', 'LAB-01', expect.anything())
    expect(listRawFilesMock).toHaveBeenCalledWith(
      { taskRunId: '5', limit: undefined, cursor: undefined },
      expect.anything(),
    )
    expect(wrapper.text()).toContain('#5')
    expect(wrapper.text()).toContain('成功')
    // tab 徽标显示摘要中的四类计数
    const badges = wrapper.findAll('.el-badge__content').map((b) => b.text())
    expect(badges).toEqual(['1', '2', '4', '1'])
  })

  it('其余 tab 懒加载：切换到解析记录才请求', async () => {
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await flushPromises()

    expect(listParsedRecordsMock).not.toHaveBeenCalled()

    await switchTab(wrapper, 'parsed-records')

    expect(listParsedRecordsMock).toHaveBeenCalledWith(
      { taskRunId: '5', limit: undefined, cursor: undefined },
      expect.anything(),
    )
    expect(listQcResultsMock).not.toHaveBeenCalled()
  })

  it('已加载过的 tab 再次切换回时不重复请求', async () => {
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await flushPromises()

    await switchTab(wrapper, 'qc-results')
    await switchTab(wrapper, 'raw-files')

    expect(listRawFilesMock).toHaveBeenCalledTimes(1)
  })

  it('tab 内独立分页：加载更多携带 cursor 追加', async () => {
    listRawFilesMock
      .mockResolvedValueOnce({
        items: [rawFileOf('1')],
        next_cursor: '1',
        has_more: true,
      })
      .mockResolvedValueOnce(pageOf([rawFileOf('2')]))
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await flushPromises()

    expect(wrapper.text()).toContain('observations-1.csv')
    await wrapper.find('.load-more .el-button').trigger('click')
    await flushPromises()

    expect(listRawFilesMock).toHaveBeenLastCalledWith(
      { taskRunId: '5', limit: undefined, cursor: '1' },
      expect.anything(),
    )
    expect(wrapper.text()).toContain('observations-2.csv')
  })

  it('payload 以格式化 JSON 展示', async () => {
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await switchTab(wrapper, 'parsed-records')

    const payload = wrapper.find('.run-evidence-drawer__payload')
    expect(payload.exists()).toBe(true)
    expect(payload.text()).toContain('"temperature": 25.5')
  })

  it('摘要请求失败显示错误横幅，重试成功后展示摘要', async () => {
    findTaskRunMock.mockRejectedValueOnce(new ApiError('network', 'connection refused'))
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await flushPromises()

    expect(wrapper.text()).toContain('网络连接失败')
    expect(wrapper.text()).toContain('connection refused')

    await wrapper.find('.error-banner .el-button').trigger('click')
    await flushPromises()

    expect(wrapper.text()).toContain('#5')
  })

  it('tab 请求失败显示各自错误横幅且不影响其他 tab', async () => {
    listAlertsMock.mockRejectedValue(new ApiError('server', 'db error'))
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await switchTab(wrapper, 'alerts')

    expect(wrapper.text()).toContain('服务端处理出错')
    expect(wrapper.text()).toContain('db error')

    await switchTab(wrapper, 'qc-results')
    expect(wrapper.text()).toContain('不通过')
  })

  it('关闭抽屉时中止在途请求', async () => {
    const signals: (AbortSignal | undefined)[] = []
    listRawFilesMock.mockImplementation((_query, signal) => {
      signals.push(signal)
      return new Promise<Page<RawFile>>(() => {})
    })
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await flushPromises()

    await wrapper.setProps({ runId: null })
    await flushPromises()

    expect(signals).toHaveLength(1)
    expect(signals[0]?.aborted).toBe(true)
    expect(wrapper.findComponent({ name: 'ElDrawer' }).props('modelValue')).toBe(
      false,
    )
  })

  it('切换查看另一运行时中止旧请求并重新加载', async () => {
    const signals: (AbortSignal | undefined)[] = []
    listRawFilesMock.mockImplementation((_query, signal) => {
      signals.push(signal)
      return new Promise<Page<RawFile>>(() => {})
    })
    const wrapper = await mountDrawer({ runId: '5', nodeCode: 'LAB-01' })
    await flushPromises()

    await wrapper.setProps({ runId: '6' })
    await flushPromises()

    expect(signals).toHaveLength(2)
    expect(signals[0]?.aborted).toBe(true)
    expect(signals[1]?.aborted).toBe(false)
    expect(findTaskRunMock).toHaveBeenLastCalledWith('6', 'LAB-01', expect.anything())
  })
})
