#pragma once

#include "labbridge/core/models.h"
#include "labbridge/core/result.h"
#include "labbridge/server/repositories/config_repository.h"
#include "labbridge/server/repositories/node_repository.h"
#include "labbridge/server/repositories/qc_repository.h"

#include <string>
#include <vector>

namespace labbridge::server {

struct ManagementDataSourceCreateRequest {
    std::string node_code;
    labbridge::core::SourceType source_type{
        labbridge::core::SourceType::LocalDirectory};
    std::string name;
    std::string config_json;
    bool enabled{true};
};

struct ManagementQcRuleCreateRequest {
    std::string name;
    std::string rule_type;
    std::string rule_config_json;
    bool enabled{true};
};

struct ManagementTaskCreateRequest {
    std::string node_code;
    std::string data_source_id;
    std::string name;
    std::string task_type;
    std::string schedule_expr;
    std::string parser_type;
    std::string qc_profile;
    std::vector<std::string> qc_rule_ids;
    bool enabled{true};
};

struct ManagementCommandResult {
    labbridge::core::Status status;
    std::string id;
};

class ManagementCommandService {
public:
    ManagementCommandService(INodeRepository& node_repository,
                             IConfigRepository& config_repository,
                             IQcRepository& qc_repository);

    ManagementCommandResult create_data_source(
        const ManagementDataSourceCreateRequest& request);
    ManagementCommandResult create_qc_rule(
        const ManagementQcRuleCreateRequest& request);
    ManagementCommandResult create_task(
        const ManagementTaskCreateRequest& request);
    ManagementCommandResult set_task_enabled(const std::string& task_id,
                                             bool enabled);

private:
    labbridge::core::Status validate_task_dependencies(
        const TaskRecord& task,
        const std::vector<std::string>& qc_rule_ids) const;

    INodeRepository& node_repository_;
    IConfigRepository& config_repository_;
    IQcRepository& qc_repository_;
};

}  // namespace labbridge::server
