#pragma once

#include "labbridge/server/agent_control_service.h"

#include <string>

namespace labbridge::server {

class PostgresAgentControlExecutor {
public:
    explicit PostgresAgentControlExecutor(std::string connection_info);

    labbridge::core::Status register_node(const labbridge::core::NodeInfo& node) const;
    labbridge::core::Status accept_heartbeat(
        const labbridge::core::NodeHeartbeat& heartbeat) const;
    AgentConfigResult find_config(const std::string& node_code) const;

private:
    std::string connection_info_;
};

}  // namespace labbridge::server
