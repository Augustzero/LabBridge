#pragma once

#include "labbridge/core/models.h"
#include "labbridge/core/result.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/repositories/task_run_repository.h"

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
    // 该节点尚未开始的人工执行（manual / retry），随配置一起下发。
    std::vector<PendingExecutionRecord> pending_executions;
};

class AgentControlService {
public:
    AgentControlService(NodeService& node_service,
                        ConfigService& config_service,
                        ITaskRunRepository& task_run_repository);

    labbridge::core::Status register_node(const labbridge::core::NodeInfo& node);
    labbridge::core::Status accept_heartbeat(
        const labbridge::core::NodeHeartbeat& heartbeat);
    AgentConfigResult find_config(const std::string& node_code) const;

private:
    NodeService& node_service_;
    ConfigService& config_service_;
    ITaskRunRepository& task_run_repository_;
};

}  // namespace labbridge::server
