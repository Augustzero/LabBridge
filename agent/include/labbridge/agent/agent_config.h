#pragma once

#include "labbridge/core/models.h"

#include <chrono>
#include <stdexcept>
#include <string>

namespace labbridge::agent {

struct AgentStartupConfig {
    labbridge::core::NodeInfo node;
    std::string server_url;
    std::chrono::milliseconds request_timeout;
};

class AgentConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

AgentStartupConfig load_agent_config(const std::string& path);

}  // namespace labbridge::agent
