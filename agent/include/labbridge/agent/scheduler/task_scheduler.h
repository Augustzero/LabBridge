#pragma once

#include "labbridge/agent/runtime/runtime_config_sink.h"

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

class ITaskExecutor {
public:
    virtual ~ITaskExecutor() = default;
    virtual void execute(ScheduledTaskExecution execution) = 0;
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
        std::vector<labbridge::core::TaskConfig> tasks) override;
    void run();
    void request_stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace labbridge::agent
