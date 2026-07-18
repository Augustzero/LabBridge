#include "labbridge/server/agent_control_service.h"

#include <utility>

namespace labbridge::server {

AgentControlService::AgentControlService(NodeService& node_service,
                                         ConfigService& config_service)
    : node_service_(node_service), config_service_(config_service) {}

labbridge::core::Status AgentControlService::register_node(
    const labbridge::core::NodeInfo& node) {
    return node_service_.register_node(node);
}

labbridge::core::Status AgentControlService::accept_heartbeat(
    const labbridge::core::NodeHeartbeat& heartbeat) {
    return node_service_.accept_heartbeat(heartbeat);
}

AgentConfigResult AgentControlService::find_config(
    const std::string& node_code) const {
    if (node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"),
                std::nullopt,
                {}};
    }

    auto node = node_service_.find_node(node_code);
    if (!node.has_value()) {
        return {labbridge::core::Status::failure(
                    labbridge::core::StatusCode::NotFound,
                    "node is not registered"),
                std::nullopt,
                {}};
    }

    AgentConfigResult result;
    result.status = labbridge::core::Status::success();
    result.node = std::move(node);
    result.enabled_tasks = config_service_.find_enabled_tasks(node_code);
    return result;
}

}  // namespace labbridge::server
