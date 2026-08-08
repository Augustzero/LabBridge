#pragma once

#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/agent/runtime/runtime_config_sink.h"
#include "labbridge/core/models.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace labbridge::agent {

class IRuntimeTimeSource {
public:
    using SteadyTimePoint = std::chrono::steady_clock::time_point;
    using SystemTimePoint = std::chrono::system_clock::time_point;

    virtual ~IRuntimeTimeSource() = default;

    virtual SteadyTimePoint steady_now() const = 0;
    virtual SystemTimePoint system_now() const = 0;
    virtual void wait_until(
        SteadyTimePoint deadline,
        const std::atomic<bool>& stop_requested) = 0;
    virtual void wake() noexcept = 0;
};

class SystemRuntimeTimeSource final : public IRuntimeTimeSource {
public:
    SteadyTimePoint steady_now() const override;
    SystemTimePoint system_now() const override;
    void wait_until(
        SteadyTimePoint deadline,
        const std::atomic<bool>& stop_requested) override;
    void wake() noexcept override;

private:
    std::mutex wait_mutex_;
    std::condition_variable wait_condition_;
};

class AgentRuntime final {
public:
    AgentRuntime(labbridge::core::NodeInfo node,
                 std::chrono::milliseconds heartbeat_interval,
                 std::chrono::milliseconds config_poll_interval,
                 IRuntimeControlClient& client,
                 PulledAgentConfig initial_config,
                 IRuntimeTimeSource& time_source,
                 IRuntimeConfigSink* config_sink = nullptr);

    PulledAgentConfig run();
    void request_stop() noexcept;

private:
    bool stop_requested() const noexcept;
    void send_heartbeat();
    void refresh_config();

    labbridge::core::NodeInfo node_;
    std::chrono::milliseconds heartbeat_interval_;
    std::chrono::milliseconds config_poll_interval_;
    IRuntimeControlClient& client_;
    PulledAgentConfig current_config_;
    IRuntimeTimeSource& time_source_;
    IRuntimeConfigSink* config_sink_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace labbridge::agent
