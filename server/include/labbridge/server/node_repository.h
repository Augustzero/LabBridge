#pragma once

#include "labbridge/core/models.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace labbridge::server {

struct NodeRecord {
    labbridge::core::NodeInfo info;
    labbridge::core::NodeStatus status{labbridge::core::NodeStatus::Offline};
    std::string last_heartbeat_at;
};

class INodeRepository {
public:
    virtual ~INodeRepository() = default;
    virtual void upsert(NodeRecord node) = 0;
    virtual std::optional<NodeRecord> find_by_code(const std::string& node_code) const = 0;
};

class InMemoryNodeRepository final : public INodeRepository {
public:
    void upsert(NodeRecord node) override;
    std::optional<NodeRecord> find_by_code(const std::string& node_code) const override;

private:
    std::unordered_map<std::string, NodeRecord> nodes_;
};

}  // namespace labbridge::server

