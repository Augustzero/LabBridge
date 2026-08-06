#include "support/server/in_memory_repositories.h"

#include <utility>

namespace labbridge::server {

void InMemoryNodeRepository::upsert(NodeRecord node) {
    nodes_[node.info.node_code] = std::move(node);
}

std::optional<NodeRecord> InMemoryNodeRepository::find_by_code(const std::string& node_code) const {
    const auto iter = nodes_.find(node_code);
    if (iter == nodes_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

}  // namespace labbridge::server
