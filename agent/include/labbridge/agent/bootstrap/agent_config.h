#pragma once

#include "labbridge/core/models.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace labbridge::agent {

// 容量保护默认值：单轮最多 10 个文件；单个请求体上限 900 KiB，
// 需低于 Server/Web 链路里最小的请求体上限（当前默认 1 MiB）。
constexpr std::size_t kDefaultMaxFilesPerRun = 10;
constexpr std::size_t kDefaultMaxRequestBodyBytes = 921600;

struct AgentStartupConfig {
    labbridge::core::NodeInfo node;
    std::string server_url;
    std::chrono::milliseconds request_timeout;
    std::chrono::milliseconds heartbeat_interval;
    std::chrono::milliseconds config_poll_interval;
    std::string work_dir;
    std::string queue_db;
    std::size_t max_pending_jobs{1000};
    std::size_t processed_fingerprint_capacity_per_task{10000};
    std::chrono::seconds retry_initial{2};
    std::chrono::seconds retry_max{300};
    // 单次运行最多带走的文件数与单个请求体上限；配置里不填就走默认值。
    std::size_t max_files_per_run{kDefaultMaxFilesPerRun};
    std::size_t max_request_body_bytes{kDefaultMaxRequestBodyBytes};
    std::vector<std::string> allowed_local_roots;
    // 本节点访问控制面的密钥（agent.token_file 的加载结果），
    // 只在内存中使用，不进入任务配置投影、SQLite 队列或归档内容。
    std::string auth_token;
};

class AgentConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

AgentStartupConfig parse_agent_config(std::string_view yaml_content);
AgentStartupConfig load_agent_config(const std::string& path);

}  // namespace labbridge::agent
