#pragma once

#include "labbridge/core/models.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace labbridge::agent {

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
    std::vector<std::string> allowed_local_roots;
};

class AgentConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

AgentStartupConfig parse_agent_config(std::string_view yaml_content);
AgentStartupConfig load_agent_config(const std::string& path);

}  // namespace labbridge::agent
