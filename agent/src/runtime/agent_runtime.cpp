#include "labbridge/agent/runtime/agent_runtime.h"

#include "labbridge/core/logging.h"
#include "labbridge/core/utc_time.h"

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

// 心跳/配置拉取被 401/403 拒绝时，把操作、节点和状态码记全再抛出，
// 方便运维直接定位是哪个节点的凭据需要修正。
void log_auth_rejection(std::string_view operation,
                        const ControlPlaneClientError& error,
                        const std::string& node_code) {
    std::ostringstream message;
    message << operation << " rejected by authentication"
            << "; node_code=" << node_code
            << "; http_status=" << error.http_status();
    labbridge::core::log_error(kComponent, message.str());
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
                           IRuntimeConfigSink* config_sink,
                           bool initially_connected,
                           std::chrono::milliseconds reconnect_initial,
                           std::chrono::milliseconds reconnect_max)
    : node_(std::move(node)),
      heartbeat_interval_(heartbeat_interval),
      config_poll_interval_(config_poll_interval),
      client_(client),
      current_config_(std::move(initial_config)),
      time_source_(time_source),
      config_sink_(config_sink),
      connected_(initially_connected),
      reconnect_initial_(reconnect_initial),
      reconnect_max_(reconnect_max) {
    if (heartbeat_interval_ <= std::chrono::milliseconds::zero() ||
        config_poll_interval_ <= std::chrono::milliseconds::zero() ||
        reconnect_initial_ <= std::chrono::milliseconds::zero() ||
        reconnect_max_ < reconnect_initial_) {
        throw std::invalid_argument("runtime intervals must be positive");
    }
}

void AgentRuntime::publish_initial_config() {
    if (connected_ && !initial_config_published_ && config_sink_ != nullptr) {
        config_sink_->replace_config(current_config_.tasks);
    }
    initial_config_published_ = true;
}
PulledAgentConfig AgentRuntime::run() {
    publish_initial_config();
    auto now = time_source_.steady_now();
    auto next_heartbeat = now + heartbeat_interval_;
    auto next_config_poll = now + config_poll_interval_;
    auto reconnect_delay = reconnect_initial_;
    auto next_reconnect = now;

    while (!stop_requested()) {
        now = time_source_.steady_now();
        if (!connected_) {
            if (now >= next_reconnect) {
                try {
                    if (reconnect()) {
                        connected_ = true;
                        reconnect_delay = reconnect_initial_;
                        now = time_source_.steady_now();
                        next_heartbeat = now + heartbeat_interval_;
                        next_config_poll = now + config_poll_interval_;
                        continue;
                    }
                } catch (const ControlPlaneClientError& error) {
                    // 认证被拒跟其他非瞬时错误一样直接抛出终止进程：
                    // 反复重连救不了配错的凭据。
                    if (error.is_auth_rejection() || !error.is_transient()) {
                        throw;
                    }
                    log_control_plane_failure("reconnect", error);
                }
                next_reconnect = time_source_.steady_now() + reconnect_delay;
                reconnect_delay = std::min(reconnect_max_, reconnect_delay * 2);
            }
            time_source_.wait_until(next_reconnect, stop_requested_);
            continue;
        }

        if (now >= next_heartbeat) {
            send_heartbeat();
            next_heartbeat = time_source_.steady_now() + heartbeat_interval_;
        }
        if (stop_requested()) {
            break;
        }
        if (time_source_.steady_now() >= next_config_poll) {
            refresh_config();
            next_config_poll = time_source_.steady_now() + config_poll_interval_;
        }
        if (stop_requested()) {
            break;
        }
        time_source_.wait_until(std::min(next_heartbeat, next_config_poll),
                                stop_requested_);
    }
    return current_config_;
}

bool AgentRuntime::reconnect() {
    client_.register_node(node_);
    if (stop_requested()) {
        return false;
    }
    client_.send_heartbeat({node_.node_code, node_.agent_version,
                            labbridge::core::format_utc_timestamp(time_source_.system_now())});
    if (stop_requested()) {
        return false;
    }
    current_config_ = client_.fetch_config(node_.node_code);
    if (config_sink_ != nullptr) {
        config_sink_->replace_config(current_config_.tasks);
    }
    initial_config_published_ = true;
    labbridge::core::log_info(
        kComponent, "control plane reconnected; enabled_tasks=" +
                        std::to_string(current_config_.tasks.size()));
    return true;
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
            labbridge::core::format_utc_timestamp(time_source_.system_now()),
        });
    } catch (const ControlPlaneClientError& error) {
        // 凭据问题重试无意义，记日志后抛出让进程停下；其他错误照旧下个周期再试。
        if (error.is_auth_rejection()) {
            log_auth_rejection("heartbeat", error, node_.node_code);
            throw;
        }
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
        if (error.is_auth_rejection()) {
            log_auth_rejection("config fetch", error, node_.node_code);
            throw;
        }
        // 拉取失败时保留最后一次成功快照，下一周期再尝试替换。
        log_control_plane_failure("config fetch", error);
    }
}

}  // namespace labbridge::agent
