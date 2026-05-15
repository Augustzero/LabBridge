#pragma once

#include "labbridge/core/models.h"
#include "labbridge/core/result.h"
#include "labbridge/server/node_repository.h"

#include <string>

namespace labbridge::server {

class NodeService {
public:
    explicit NodeService(INodeRepository& repository);

    labbridge::core::Status register_node(const labbridge::core::NodeInfo& node);
    labbridge::core::Status accept_heartbeat(const labbridge::core::NodeHeartbeat& heartbeat);
    std::optional<NodeRecord> find_node(const std::string& node_code) const;

private:
    INodeRepository& repository_;
};

}  // namespace labbridge::server

