#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/agent/execution/sha256.h"

#include <string_view>

namespace labbridge::agent {

TaskExecutionClientError::TaskExecutionClientError(
    TaskExecutionErrorKind kind, std::string message, unsigned int http_status)
    : std::runtime_error(std::move(message)), kind_(kind),
      http_status_(http_status) {}

TaskExecutionErrorKind TaskExecutionClientError::kind() const noexcept {
    return kind_;
}

unsigned int TaskExecutionClientError::http_status() const noexcept {
    return http_status_;
}

bool TaskExecutionClientError::is_transient() const noexcept {
    return kind_ == TaskExecutionErrorKind::Network ||
           kind_ == TaskExecutionErrorKind::ServerError ||
           http_status_ == 408 || http_status_ == 429 ||
           (http_status_ >= 500 && http_status_ <= 599);
}

// 服务端的 401/403 走统一错误 envelope，到客户端会被归成 ServerError，
// 仅凭 is_transient() 区分不出来，所以这里按状态码单独判定。
bool TaskExecutionClientError::is_auth_rejection() const noexcept {
    return http_status_ == 401 || http_status_ == 403;
}

std::string make_scheduled_execution_key(
    const std::string& node_code,
    const std::string& task_id,
    const std::string& scheduled_for) {
    return sha256_hex(
        "scheduled\n" + node_code + "\n" + task_id + "\n" + scheduled_for);
}

std::string make_manifest_idempotency_key(
    const std::string& node_code,
    const std::string& task_run_id) {
    return sha256_hex("manifest\n" + node_code + "\n" + task_run_id);
}

std::string make_report_idempotency_key(
    const std::string& node_code,
    const std::string& task_run_id) {
    return sha256_hex("report\n" + node_code + "\n" + task_run_id);
}

}  // namespace labbridge::agent
