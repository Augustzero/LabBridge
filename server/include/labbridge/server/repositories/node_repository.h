#pragma once

#include "labbridge/core/models.h"

#include <optional>
#include <string>

namespace labbridge::server {

struct NodeRecord {
    labbridge::core::NodeInfo info;
    labbridge::core::NodeStatus status{labbridge::core::NodeStatus::Offline};
    std::string last_heartbeat_at;
    std::string id;
    std::string created_at;
    std::string updated_at;
};

class INodeRepository {
public:
    virtual ~INodeRepository() = default;
    virtual void upsert(NodeRecord node) = 0;
    virtual std::optional<NodeRecord> find_by_code(const std::string& node_code) const = 0;
};

}  // namespace labbridge::server

