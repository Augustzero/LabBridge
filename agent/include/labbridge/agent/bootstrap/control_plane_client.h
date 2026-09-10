#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/core/models.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace labbridge::agent {

enum class ControlPlaneErrorKind {
    Network,
    HttpStatus,
    InvalidJson,
    InvalidResponse,
    ServerError,
};

class ControlPlaneClientError final : public TaskExecutionClientError {
public:
    ControlPlaneClientError(ControlPlaneErrorKind kind,
                            std::string message,
                            unsigned int http_status = 0,
                            std::string server_code = {});

    ControlPlaneErrorKind kind() const noexcept;
    unsigned int http_status() const noexcept;
    const std::string& server_code() const noexcept;

private:
    ControlPlaneErrorKind kind_;
    unsigned int http_status_;
    std::string server_code_;
};

struct PulledAgentConfig {
    labbridge::core::NodeInfo node;
    labbridge::core::NodeStatus status{labbridge::core::NodeStatus::Offline};
    std::string last_heartbeat_at;
    std::vector<labbridge::core::TaskConfig> tasks;
};

void validate_control_plane_url(std::string_view server_url);

class IRuntimeControlClient {
public:
    virtual ~IRuntimeControlClient() = default;

    virtual void register_node(const labbridge::core::NodeInfo&) const {}

    virtual void send_heartbeat(
        const labbridge::core::NodeHeartbeat& heartbeat) const = 0;
    virtual PulledAgentConfig fetch_config(
        const std::string& node_code) const = 0;
};

class ControlPlaneClient final : public IRuntimeControlClient,
                                 public ITaskExecutionClient {
public:
    // 节点编号与密钥来自启动配置，构造后固定；六个请求统一附加认证头。
    ControlPlaneClient(std::string server_url,
                       std::chrono::milliseconds request_timeout,
                       std::string node_code,
                       std::string auth_token);

    void register_node(const labbridge::core::NodeInfo& node) const override;
    void send_heartbeat(
        const labbridge::core::NodeHeartbeat& heartbeat) const override;
    PulledAgentConfig fetch_config(
        const std::string& node_code) const override;
    StartTaskRunResult start_task_run(
        const StartTaskRunRequest& request) const override;
    RawFileManifestResult report_raw_file_manifest(
        const RawFileManifestRequest& request) const override;
    TaskRunReportResult report_task_run(
        const TaskRunReportRequest& request) const override;

private:
    struct HttpResponse {
        unsigned int status;
        std::string body;
    };

    HttpResponse request(std::string_view method,
                         const std::string& target,
                         std::string body = {}) const;

    std::string host_;
    std::string port_;
    std::string host_header_;
    std::string node_code_;
    std::string auth_token_;
    std::chrono::milliseconds request_timeout_;
};

}  // namespace labbridge::agent
