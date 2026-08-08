#include "labbridge/agent/scheduler/task_scheduler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

class NoopExecutor final : public labbridge::agent::ITaskExecutor {
public:
    void execute(labbridge::agent::ScheduledTaskExecution) override {
        ++calls;
    }
    int calls{0};
};

class BlockingTimeSource final
    : public labbridge::agent::ISchedulerTimeSource {
public:
    SteadyTimePoint steady_now() const override { return {}; }
    SystemTimePoint system_now() const override { return {}; }

    void wait_until(SteadyTimePoint) override {
        std::unique_lock<std::mutex> lock{mutex_};
        waiting_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return woken_; });
    }

    void wake() noexcept override {
        std::lock_guard<std::mutex> lock{mutex_};
        woken_ = true;
        condition_.notify_all();
    }

    void wait_until_blocked() {
        std::unique_lock<std::mutex> lock{mutex_};
        condition_.wait(lock, [&] { return waiting_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool waiting_{false};
    bool woken_{false};
};

TEST(TaskSchedulerLifecycleTest, StopWakesWorkerWithoutDispatchingNewTask) {
    NoopExecutor executor;
    BlockingTimeSource time;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    std::thread worker{[&] { scheduler.run(); }};

    time.wait_until_blocked();
    scheduler.request_stop();
    worker.join();

    EXPECT_EQ(executor.calls, 0);
}

TEST(TaskSchedulerLifecycleTest, WakeBeforeWaitIsNotLost) {
    labbridge::agent::SystemSchedulerTimeSource time;

    time.wake();
    time.wait_until(time.steady_now() + std::chrono::hours{1});

    SUCCEED();
}

}  // namespace
