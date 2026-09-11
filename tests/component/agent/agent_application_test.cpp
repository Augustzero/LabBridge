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

// 两个控制面入口都要让异常走到 application，唤醒并收回调度线程。
class RejectingControlClient final : public labbridge::agent::IRuntimeControlClient {
public:
    RejectingControlClient(bool reject_heartbeat, unsigned int status)
        : reject_heartbeat_(reject_heartbeat), status_(status) {}

    void send_heartbeat(const labbridge::core::NodeHeartbeat&) const override {
        if (reject_heartbeat_) {
            reject();
        }
    }

    labbridge::agent::PulledAgentConfig fetch_config(const std::string&) const override {
        reject();
    }

private:
    [[noreturn]] void reject() const {
        throw labbridge::agent::ControlPlaneClientError{
            labbridge::agent::ControlPlaneErrorKind::ServerError,
            "credential rejected", status_, "unauthenticated"};
    }

    bool reject_heartbeat_;
    unsigned int status_;
};

TEST(AgentApplicationTest, ControlAuthenticationFailureJoinsScheduler) {
    for (const bool heartbeat : {true, false}) {
        for (const unsigned int status : {401U, 403U}) {
            SCOPED_TRACE(status);
            SCOPED_TRACE(heartbeat);
            RejectingControlClient client{heartbeat, status};
            labbridge::agent::SystemRuntimeTimeSource runtime_time;
            BlockingSchedulerTimeSource scheduler_time;
            NoopExecutor executor;
            labbridge::agent::TaskScheduler scheduler{executor, scheduler_time};
            labbridge::agent::AgentRuntime runtime{
                {"phase027-node", "auth lifecycle", "0.1.0"},
                heartbeat ? 1ms : 1h, heartbeat ? 1h : 1ms,
                client, {}, runtime_time, &scheduler};
            labbridge::agent::AgentApplication application{runtime, scheduler};

            try {
                static_cast<void>(application.run());
                FAIL() << "authentication failure should reach the process boundary";
            } catch (const labbridge::agent::ControlPlaneClientError& error) {
                EXPECT_EQ(error.http_status(), status);
            }
            EXPECT_EQ(executor.stop_requests.load(), 1);
        }
    }
}

class AuthFailingExecutor final : public labbridge::agent::ITaskExecutor {
public:
    explicit AuthFailingExecutor(unsigned int status) : status_(status) {}

    void execute(labbridge::agent::ScheduledTaskExecution) override {
        throw labbridge::agent::TaskExecutionClientError{
            labbridge::agent::TaskExecutionErrorKind::ServerError,
            "delivery credential rejected", status_};
    }

    void request_stop() noexcept override { stopped = true; }

    bool stopped{false};

private:
    unsigned int status_;
};

TEST(AgentApplicationTest, DeliveryAuthenticationFailureWakesControlLoop) {
    for (const unsigned int status : {401U, 403U}) {
        FakeControlClient client;
        BlockingRuntimeTimeSource runtime_time;
        AdvancingSchedulerTimeSource scheduler_time;
        AuthFailingExecutor executor{status};
        labbridge::agent::TaskScheduler scheduler{executor, scheduler_time};
        labbridge::agent::AgentRuntime runtime{
            {"phase022-node", "auth lifecycle", "0.1.0"}, 1h, 1h,
            client, initial_config_with_task(), runtime_time, &scheduler};
        labbridge::agent::AgentApplication application{runtime, scheduler};

        try {
            static_cast<void>(application.run());
            FAIL() << "delivery rejection should reach the process boundary";
        } catch (const labbridge::agent::TaskExecutionClientError& error) {
            EXPECT_EQ(error.http_status(), status);
        }
        EXPECT_TRUE(executor.stopped);
    }
}

}  // namespace
