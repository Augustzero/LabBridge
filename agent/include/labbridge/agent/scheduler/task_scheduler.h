#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/agent/runtime/runtime_config_sink.h"
#include "labbridge/core/models.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace labbridge::agent {

struct ScheduledTaskExecution {
    labbridge::core::TaskConfig task;
    std::chrono::system_clock::time_point scheduled_for;
};

// 配置轮询下发的人工候选与当前任务配置的配对。
struct ManualTaskExecution {
    PendingExecution execution;
    labbridge::core::TaskConfig task;
};

// execute_pending 的派发结果：调度器据此决定本轮是否继续取候选。
enum class PendingDispatchResult {
    Dispatched,     // 已入队并执行
    AlreadyQueued,  // 本地已有同键作业（含 retry_wait / attention）
    QueueFull,      // 队列已满：请求留在中心 pending，等下次唤醒再试
};

class ITaskExecutor {
public:
    virtual ~ITaskExecutor() = default;
    virtual void recover_pending_jobs() {}
    virtual void execute(ScheduledTaskExecution execution) = 0;
    virtual PendingDispatchResult execute_pending(
        ManualTaskExecution execution) = 0;
    virtual void request_stop() noexcept = 0;
};

class ISchedulerTimeSource {
public:
    using SteadyTimePoint = std::chrono::steady_clock::time_point;
    using SystemTimePoint = std::chrono::system_clock::time_point;

    virtual ~ISchedulerTimeSource() = default;
    virtual SteadyTimePoint steady_now() const = 0;
    virtual SystemTimePoint system_now() const = 0;
    virtual void wait_until(SteadyTimePoint deadline) = 0;
    virtual void wake() noexcept = 0;
};

class SystemSchedulerTimeSource final : public ISchedulerTimeSource {
public:
    SteadyTimePoint steady_now() const override;
    SystemTimePoint system_now() const override;
    void wait_until(SteadyTimePoint deadline) override;
    void wake() noexcept override;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool wake_pending_{false};
};

class TaskScheduler final : public IRuntimeConfigSink {
public:
    TaskScheduler(ITaskExecutor& executor, ISchedulerTimeSource& time_source);
    ~TaskScheduler();
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    void replace_config(
        std::vector<labbridge::core::TaskConfig> tasks,
        std::vector<PendingExecution> pending_executions) override;
    void run();
    void request_stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace labbridge::agent
