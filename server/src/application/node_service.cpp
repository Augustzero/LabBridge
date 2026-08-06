#include "labbridge/server/application/node_service.h"

#include <utility>

namespace labbridge::server {

NodeService::NodeService(INodeRepository& repository) : repository_(repository) {}

labbridge::core::Status NodeService::register_node(const labbridge::core::NodeInfo& node) {
    if (node.node_code.empty()) {
        return labbridge::core::Status::failure("node_code is required");
    }
    if (node.name.empty()) {
        return labbridge::core::Status::failure("node name is required");
    }

    NodeRecord record;
    record.info = node;
    record.status = labbridge::core::NodeStatus::Offline;
    repository_.upsert(std::move(record));
    return labbridge::core::Status::success();
}

labbridge::core::Status NodeService::accept_heartbeat(const labbridge::core::NodeHeartbeat& heartbeat) {
    if (heartbeat.node_code.empty()) {
        return labbridge::core::Status::failure("node_code is required");
    }
    if (heartbeat.reported_at.empty()) {
        return labbridge::core::Status::failure("reported_at is required");
    }

    auto node = repository_.find_by_code(heartbeat.node_code);
    if (!node.has_value()) {
        return labbridge::core::Status::failure(
            labbridge::core::StatusCode::NotFound,
            "node is not registered");
    }

    node->status = labbridge::core::NodeStatus::Online;
    node->info.agent_version = heartbeat.agent_version;
    node->last_heartbeat_at = heartbeat.reported_at;
    repository_.upsert(*node);
    return labbridge::core::Status::success();
}

std::optional<NodeRecord> NodeService::find_node(const std::string& node_code) const {
    return repository_.find_by_code(node_code);
}

}  // namespace labbridge::server

