#pragma once

#include "labbridge/core/models.h"
#include "labbridge/core/result.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"

#include <optional>
#include <string>
#include <vector>

namespace labbridge::server {

struct AgentConfigResult {
    labbridge::core::Status status;
    std::optional<NodeRecord> node;
    std::vector<TaskRecord> enabled_tasks;
    std::vector<DataSourceRecord> data_sources;
    std::vector<TaskQcRuleBinding> task_qc_rules;
};

class AgentControlService {
public:
    AgentControlService(NodeService& node_service, ConfigService& config_service);

    labbridge::core::Status register_node(const labbridge::core::NodeInfo& node);
    labbridge::core::Status accept_heartbeat(
        const labbridge::core::NodeHeartbeat& heartbeat);
    AgentConfigResult find_config(const std::string& node_code) const;

private:
    NodeService& node_service_;
    ConfigService& config_service_;
};

}  // namespace labbridge::server
