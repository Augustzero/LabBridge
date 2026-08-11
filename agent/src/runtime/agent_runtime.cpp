#include "labbridge/agent/runtime/agent_runtime.h"

#include "labbridge/agent/bootstrap/utc_time.h"
#include "labbridge/core/logging.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace labbridge::agent {
namespace {

constexpr std::string_view kComponent = "agent-runtime";

const char* error_kind_name(ControlPlaneErrorKind kind) {
    switch (kind) {
        case ControlPlaneErrorKind::Network:
            return "network";
        case ControlPlaneErrorKind::HttpStatus:
            return "http_status";
        case ControlPlaneErrorKind::InvalidJson:
            return "invalid_json";
        case ControlPlaneErrorKind::InvalidResponse:
            return "invalid_response";
        case ControlPlaneErrorKind::ServerError:
            return "server_error";
    }
    return "unknown";
}

void log_control_plane_failure(std::string_view operation,
                               const ControlPlaneClientError& error) {
    std::ostringstream message;
    message << operation << " failed; kind=" << error_kind_name(error.kind())
            << "; http_status=" << error.http_status();
    if (!error.server_code().empty()) {
        message << "; server_code=" << error.server_code();
    }
    message << "; message=" << error.what();
    labbridge::core::log_warn(kComponent, message.str());
}

}  // namespace

IRuntimeTimeSource::SteadyTimePoint SystemRuntimeTimeSource::steady_now() const {
    return std::chrono::steady_clock::now();
}

IRuntimeTimeSource::SystemTimePoint SystemRuntimeTimeSource::system_now() const {
    return std::chrono::system_clock::now();
}

void SystemRuntimeTimeSource::wait_until(
    SteadyTimePoint deadline,
    const std::atomic<bool>& stop_requested) {
    std::unique_lock<std::mutex> lock{wait_mutex_};
    wait_condition_.wait_until(lock, deadline, [&stop_requested] {
        return stop_requested.load(std::memory_order_acquire);
    });
}

void SystemRuntimeTimeSource::wake() noexcept {
    wait_condition_.notify_all();
}

AgentRuntime::AgentRuntime(labbridge::core::NodeInfo node,
                           std::chrono::milliseconds heartbeat_interval,
                           std::chrono::milliseconds config_poll_interval,
                           IRuntimeControlClient& client,
                           PulledAgentConfig initial_config,
                           IRuntimeTimeSource& time_source,
                           IRuntimeConfigSink* config_sink)
    : node_(std::move(node)),
      heartbeat_interval_(heartbeat_interval),
      config_poll_interval_(config_poll_interval),
      client_(client),
      current_config_(std::move(initial_config)),
      time_source_(time_source),
      config_sink_(config_sink) {
    if (heartbeat_interval_ <= std::chrono::milliseconds::zero() ||
        config_poll_interval_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("runtime intervals must be positive");
    }
}

void AgentRuntime::publish_initial_config() {
    if (!initial_config_published_ && config_sink_ != nullptr) {
        config_sink_->replace_config(current_config_.tasks);
    }
    initial_config_published_ = true;
}

PulledAgentConfig AgentRuntime::run() {
    publish_initial_config();
    const auto started_at = time_source_.steady_now();
    auto next_heartbeat = started_at + heartbeat_interval_;
    auto next_config_poll = started_at + config_poll_interval_;

    while (!stop_requested()) {
        if (time_source_.steady_now() >= next_heartbeat) {
            if (stop_requested()) {
                break;
            }
            send_heartbeat();
            next_heartbeat = time_source_.steady_now() + heartbeat_interval_;
        }

        // 请求期间可能收到停止通知，不能继续发起同一轮的下一项网络请求。
        if (stop_requested()) {
            break;
        }

        if (time_source_.steady_now() >= next_config_poll) {
            if (stop_requested()) {
                break;
            }
            refresh_config();
            next_config_poll =
                time_source_.steady_now() + config_poll_interval_;
        }

        if (stop_requested()) {
            break;
        }

        time_source_.wait_until(
            std::min(next_heartbeat, next_config_poll), stop_requested_);
    }

    return current_config_;
}

void AgentRuntime::request_stop() noexcept {
    const bool was_stopped =
        stop_requested_.exchange(true, std::memory_order_acq_rel);
    if (!was_stopped) {
        time_source_.wake();
    }
}

bool AgentRuntime::stop_requested() const noexcept {
    return stop_requested_.load(std::memory_order_acquire);
}

void AgentRuntime::send_heartbeat() {
    try {
        client_.send_heartbeat({
            node_.node_code,
            node_.agent_version,
            format_utc_timestamp(time_source_.system_now()),
        });
    } catch (const ControlPlaneClientError& error) {
        log_control_plane_failure("heartbeat", error);
    }
}

void AgentRuntime::refresh_config() {
    try {
        auto pulled_config = client_.fetch_config(node_.node_code);
        current_config_ = std::move(pulled_config);
        if (config_sink_ != nullptr) {
            config_sink_->replace_config(current_config_.tasks);
        }
        labbridge::core::log_info(
            kComponent,
            "config updated; enabled_tasks=" +
                std::to_string(current_config_.tasks.size()));
    } catch (const ControlPlaneClientError& error) {
        // 拉取失败时保留最后一次成功快照，下一周期再尝试替换。
        log_control_plane_failure("config fetch", error);
    }
}

}  // namespace labbridge::agent
