#include "labbridge/agent/runtime/agent_application.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;

class FakeControlClient final : public labbridge::agent::IRuntimeControlClient {
public:
    void send_heartbeat(
        const labbridge::core::NodeHeartbeat&) const override {}
    labbridge::agent::PulledAgentConfig fetch_config(
        const std::string&) const override {
        return {};
    }
};

class BlockingRuntimeTimeSource final
    : public labbridge::agent::IRuntimeTimeSource {
public:
    SteadyTimePoint steady_now() const override { return {}; }
    SystemTimePoint system_now() const override { return SystemTimePoint{30s}; }

    void wait_until(
        SteadyTimePoint,
        const std::atomic<bool>& stop_requested) override {
        std::unique_lock<std::mutex> lock{mutex_};
        waiting_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] {
            return woken_ ||
                   stop_requested.load(std::memory_order_acquire);
        });
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

class BlockingSchedulerTimeSource final
    : public labbridge::agent::ISchedulerTimeSource {
public:
    SteadyTimePoint steady_now() const override { return {}; }
    SystemTimePoint system_now() const override { return SystemTimePoint{30s}; }

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

class NoopExecutor final : public labbridge::agent::ITaskExecutor {
public:
    void execute(labbridge::agent::ScheduledTaskExecution) override {
        ++executions;
    }
    void request_stop() noexcept override { ++stop_requests; }
    void forget_task(const std::string&) override {}

    std::atomic<int> executions{0};
    std::atomic<int> stop_requests{0};
};

labbridge::agent::PulledAgentConfig initial_config_with_task() {
    labbridge::agent::PulledAgentConfig config;
    labbridge::core::TaskConfig task;
    task.id = "30";
    task.node_code = "phase022-node";
    task.schedule_expr = "* * * * *";
    task.enabled = true;
    config.tasks.push_back(std::move(task));
    return config;
}

TEST(AgentApplicationTest, IdleStopJoinsControlAndSchedulerLoops) {
    FakeControlClient client;
    BlockingRuntimeTimeSource runtime_time;
    BlockingSchedulerTimeSource scheduler_time;
    NoopExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, scheduler_time};
    labbridge::agent::AgentRuntime runtime{
        {"phase022-node", "phase 022 node", "0.22.0"},
        1h,
        1h,
        client,
        {},
        runtime_time,
        &scheduler};
    labbridge::agent::AgentApplication application{runtime, scheduler};

    std::thread process{[&] {
        static_cast<void>(application.run());
    }};
    runtime_time.wait_until_blocked();
    scheduler_time.wait_until_blocked();

    application.request_stop();
    process.join();

    EXPECT_EQ(executor.executions.load(), 0);
    EXPECT_EQ(executor.stop_requests.load(), 1);
    std::cout << "application_idle_stop control_loop=joined "
              << "scheduler_worker=joined new_task_runs=0" << std::endl;
}

class AdvancingSchedulerTimeSource final
    : public labbridge::agent::ISchedulerTimeSource {
public:
    SteadyTimePoint steady_now() const override { return steady_; }
    SystemTimePoint system_now() const override { return system_; }

    void wait_until(SteadyTimePoint deadline) override {
        steady_ = deadline;
        system_ = SystemTimePoint{60s};
    }
    void wake() noexcept override {}

private:
    SteadyTimePoint steady_{};
    SystemTimePoint system_{30s};
};

class ThrowingExecutor final : public labbridge::agent::ITaskExecutor {
public:
    void execute(labbridge::agent::ScheduledTaskExecution execution) override {
        observed_task_id = execution.task.id;
        throw std::logic_error("unexpected worker failure");
    }
    void request_stop() noexcept override { stop_requested = true; }
    void forget_task(const std::string&) override {}

    std::string observed_task_id;
    bool stop_requested{false};
};

TEST(AgentApplicationTest, WorkerFailureStopsRuntimeAndPropagates) {
    FakeControlClient client;
    BlockingRuntimeTimeSource runtime_time;
    AdvancingSchedulerTimeSource scheduler_time;
    ThrowingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, scheduler_time};
    labbridge::agent::AgentRuntime runtime{
        {"phase022-node", "phase 022 node", "0.22.0"},
        1h,
        1h,
        client,
        initial_config_with_task(),
        runtime_time,
        &scheduler};
    labbridge::agent::AgentApplication application{runtime, scheduler};

    EXPECT_THROW(
        static_cast<void>(application.run()),
        std::logic_error);
    EXPECT_EQ(executor.observed_task_id, "30");
    EXPECT_TRUE(executor.stop_requested);
    std::cout << "worker_failure task_id=30 control_loop=stopped "
              << "process_result=nonzero_boundary" << std::endl;
}

}  // namespace
