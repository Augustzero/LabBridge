#include "labbridge/agent/runtime/agent_runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using SteadyTimePoint = labbridge::agent::IRuntimeTimeSource::SteadyTimePoint;
using SystemTimePoint = labbridge::agent::IRuntimeTimeSource::SystemTimePoint;

labbridge::core::NodeInfo test_node() {
    return {"node-021", "phase 021 node", "0.21.0"};
}

labbridge::agent::PulledAgentConfig config_with_task(std::string task_id) {
    labbridge::agent::PulledAgentConfig config;
    config.tasks.push_back({std::move(task_id)});
    return config;
}

class FakeRuntimeTimeSource final
    : public labbridge::agent::IRuntimeTimeSource {
public:
    SteadyTimePoint steady_now() const override { return steady_now_; }
    SystemTimePoint system_now() const override { return system_now_; }

    void wait_until(SteadyTimePoint deadline,
                    const std::atomic<bool>&) override {
        wait_deadlines.push_back(deadline);
        if (on_wait) {
            on_wait(deadline);
        } else {
            steady_now_ = deadline;
        }
    }

    void wake() noexcept override { ++wake_count; }
    void advance_to(SteadyTimePoint value) { steady_now_ = value; }
    void advance_by(std::chrono::milliseconds value) { steady_now_ += value; }

    SteadyTimePoint steady_now_{};
    SystemTimePoint system_now_{std::chrono::seconds{1'704'067'200}};
    std::vector<SteadyTimePoint> wait_deadlines;
    std::function<void(SteadyTimePoint)> on_wait;
    std::size_t wake_count{0};
};

class FakeRuntimeClient final
    : public labbridge::agent::IRuntimeControlClient {
public:
    void send_heartbeat(
        const labbridge::core::NodeHeartbeat& heartbeat) const override {
        calls.push_back("heartbeat");
        heartbeats.push_back(heartbeat);
        if (on_heartbeat) {
            on_heartbeat();
        }
        if (heartbeat_error) {
            throw labbridge::agent::ControlPlaneClientError{
                labbridge::agent::ControlPlaneErrorKind::Network,
                "heartbeat unavailable"};
        }
        if (unexpected_heartbeat_error) {
            throw std::logic_error{"unexpected heartbeat failure"};
        }
    }

    labbridge::agent::PulledAgentConfig fetch_config(
        const std::string& node_code) const override {
        calls.push_back("config");
        fetched_node_codes.push_back(node_code);
        if (config_errors_remaining > 0) {
            --config_errors_remaining;
            throw labbridge::agent::ControlPlaneClientError{
                labbridge::agent::ControlPlaneErrorKind::ServerError,
                "config unavailable",
                503,
                "temporarily_unavailable"};
        }
        if (configs.empty()) {
            return {};
        }
        const auto index = std::min(config_index, configs.size() - 1);
        ++config_index;
        return configs[index];
    }

    mutable std::vector<std::string> calls;
    mutable std::vector<labbridge::core::NodeHeartbeat> heartbeats;
    mutable std::vector<std::string> fetched_node_codes;
    mutable std::size_t config_index{0};
    mutable int config_errors_remaining{0};
    bool heartbeat_error{false};
    bool unexpected_heartbeat_error{false};
    std::function<void()> on_heartbeat;
    std::vector<labbridge::agent::PulledAgentConfig> configs;
};

class RuntimeScenario {
public:
    RuntimeScenario(
        std::chrono::milliseconds heartbeat_interval,
        std::chrono::milliseconds config_poll_interval,
        labbridge::agent::PulledAgentConfig initial_config =
            config_with_task("initial"))
        : runtime{test_node(),
                  heartbeat_interval,
                  config_poll_interval,
                  client,
                  std::move(initial_config),
                  time_source} {}

    void stop_on_wait(std::size_t wait_number) {
        time_source.on_wait = [this, wait_number](SteadyTimePoint deadline) {
            if (time_source.wait_deadlines.size() == wait_number) {
                runtime.request_stop();
            } else {
                time_source.advance_to(deadline);
            }
        };
    }

    FakeRuntimeClient client;
    FakeRuntimeTimeSource time_source;
    labbridge::agent::AgentRuntime runtime;
};

TEST(AgentRuntimeTest, StartsBothDeadlinesAfterHandshakeCompletion) {
    RuntimeScenario scenario{10s, 15s};
    scenario.time_source.advance_to(SteadyTimePoint{37s});
    scenario.stop_on_wait(1);

    static_cast<void>(scenario.runtime.run());

    ASSERT_EQ(scenario.time_source.wait_deadlines.size(), 1U);
    EXPECT_EQ(scenario.time_source.wait_deadlines.front(),
              SteadyTimePoint{47s});
    EXPECT_TRUE(scenario.client.calls.empty());
}

TEST(AgentRuntimeTest, UsesIndependentDeadlinesAndUtcHeartbeatTime) {
    RuntimeScenario scenario{10s, 15s};
    scenario.client.configs.push_back(config_with_task("updated"));
    scenario.stop_on_wait(4);

    const auto final_config = scenario.runtime.run();

    EXPECT_EQ(scenario.client.calls,
              (std::vector<std::string>{"heartbeat", "config", "heartbeat"}));
    ASSERT_EQ(scenario.client.heartbeats.size(), 2U);
    EXPECT_EQ(scenario.client.heartbeats.front().node_code, "node-021");
    EXPECT_EQ(scenario.client.heartbeats.front().agent_version, "0.21.0");
    EXPECT_EQ(scenario.client.heartbeats.front().reported_at,
              "2024-01-01T00:00:00Z");
    ASSERT_EQ(final_config.tasks.size(), 1U);
    EXPECT_EQ(final_config.tasks.front().id, "updated");
}

TEST(AgentRuntimeTest, SendsHeartbeatBeforeConfigWhenBothAreDue) {
    RuntimeScenario scenario{10s, 10s};
    scenario.stop_on_wait(2);

    static_cast<void>(scenario.runtime.run());

    EXPECT_EQ(scenario.client.calls,
              (std::vector<std::string>{"heartbeat", "config"}));
}

TEST(AgentRuntimeTest, ControlPlaneFailuresDoNotBlockOtherOperations) {
    RuntimeScenario scenario{10s, 10s};
    scenario.client.heartbeat_error = true;
    scenario.client.config_errors_remaining = 1;
    scenario.client.configs.push_back(config_with_task("recovered"));
    scenario.stop_on_wait(3);

    const auto final_config = scenario.runtime.run();

    EXPECT_EQ(scenario.client.calls,
              (std::vector<std::string>{
                  "heartbeat", "config", "heartbeat", "config"}));
    ASSERT_EQ(final_config.tasks.size(), 1U);
    EXPECT_EQ(final_config.tasks.front().id, "recovered");
}

TEST(AgentRuntimeTest, FailedConfigFetchPreservesLastSuccessfulSnapshot) {
    RuntimeScenario scenario{1h, 10s};
    scenario.client.config_errors_remaining = 2;
    scenario.stop_on_wait(3);

    const auto final_config = scenario.runtime.run();

    ASSERT_EQ(final_config.tasks.size(), 1U);
    EXPECT_EQ(final_config.tasks.front().id, "initial");
}

TEST(AgentRuntimeTest, SchedulesNextAttemptFromOperationCompletion) {
    RuntimeScenario scenario{10s, 1h};
    scenario.client.heartbeat_error = true;
    scenario.client.on_heartbeat = [&scenario] {
        scenario.time_source.advance_by(7s);
    };
    scenario.stop_on_wait(2);

    static_cast<void>(scenario.runtime.run());

    ASSERT_EQ(scenario.time_source.wait_deadlines.size(), 2U);
    EXPECT_EQ(scenario.time_source.wait_deadlines[0], SteadyTimePoint{10s});
    EXPECT_EQ(scenario.time_source.wait_deadlines[1], SteadyTimePoint{27s});
    EXPECT_EQ(scenario.client.calls,
              (std::vector<std::string>{"heartbeat"}));
}

TEST(AgentRuntimeTest, StopRequestedBeforeRunStartsMakesNoCalls) {
    RuntimeScenario scenario{10s, 10s};

    scenario.runtime.request_stop();
    scenario.runtime.request_stop();
    const auto final_config = scenario.runtime.run();

    EXPECT_TRUE(scenario.client.calls.empty());
    EXPECT_TRUE(scenario.time_source.wait_deadlines.empty());
    EXPECT_EQ(scenario.time_source.wake_count, 1U);
    ASSERT_EQ(final_config.tasks.size(), 1U);
    EXPECT_EQ(final_config.tasks.front().id, "initial");
}

TEST(AgentRuntimeTest, StopDuringHeartbeatPreventsDueConfigFetch) {
    RuntimeScenario scenario{10s, 10s};
    scenario.client.on_heartbeat = [&scenario] {
        scenario.runtime.request_stop();
    };
    scenario.time_source.on_wait = [&scenario](SteadyTimePoint deadline) {
        scenario.time_source.advance_to(deadline);
    };

    static_cast<void>(scenario.runtime.run());

    EXPECT_EQ(scenario.client.calls,
              (std::vector<std::string>{"heartbeat"}));
}

TEST(AgentRuntimeTest, UnexpectedClientExceptionPropagates) {
    RuntimeScenario scenario{10s, 1h};
    scenario.client.unexpected_heartbeat_error = true;
    scenario.time_source.on_wait = [&scenario](SteadyTimePoint deadline) {
        scenario.time_source.advance_to(deadline);
    };

    EXPECT_THROW(static_cast<void>(scenario.runtime.run()), std::logic_error);
}

class BlockingRuntimeTimeSource final
    : public labbridge::agent::IRuntimeTimeSource {
public:
    SteadyTimePoint steady_now() const override { return {}; }
    SystemTimePoint system_now() const override { return {}; }

    void wait_until(SteadyTimePoint,
                    const std::atomic<bool>& stop_requested) override {
        std::unique_lock<std::mutex> lock{mutex_};
        waiting_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] {
            return woken_ || stop_requested.load(std::memory_order_acquire);
        });
    }

    void wake() noexcept override {
        std::lock_guard<std::mutex> lock{mutex_};
        woken_ = true;
        condition_.notify_all();
    }

    void wait_until_runtime_is_waiting() {
        std::unique_lock<std::mutex> lock{mutex_};
        condition_.wait(lock, [&] { return waiting_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool waiting_{false};
    bool woken_{false};
};

TEST(AgentRuntimeTest, StopWakesAnActiveWaitWithoutStartingRequests) {
    FakeRuntimeClient client;
    BlockingRuntimeTimeSource time_source;
    labbridge::agent::AgentRuntime runtime{
        test_node(), 10s, 15s, client, config_with_task("initial"), time_source};
    std::thread runtime_thread{[&runtime] {
        static_cast<void>(runtime.run());
    }};

    time_source.wait_until_runtime_is_waiting();
    runtime.request_stop();
    runtime_thread.join();

    EXPECT_TRUE(client.calls.empty());
}

}  // namespace
