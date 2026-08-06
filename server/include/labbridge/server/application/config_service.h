#pragma once

#include "labbridge/core/models.h"
#include "labbridge/core/result.h"
#include "labbridge/server/repositories/config_repository.h"
#include "labbridge/server/repositories/node_repository.h"

#include <string>
#include <vector>

namespace labbridge::server {

struct CreateDataSourceRequest {
    std::string node_code;
    labbridge::core::SourceType source_type{labbridge::core::SourceType::LocalDirectory};
    std::string name;
    std::string config_json;
    bool enabled{true};
};

struct CreateTaskRequest {
    std::string node_code;
    std::string data_source_id;
    std::string name;
    std::string task_type;
    std::string schedule_expr;
    std::string parser_type;
    std::string qc_profile;
    bool enabled{true};
};

struct ConfigCreateResult {
    labbridge::core::Status status;
    std::string id;
};

class ConfigService {
public:
    ConfigService(INodeRepository& node_repository, IConfigRepository& config_repository);

    ConfigCreateResult create_data_source(const CreateDataSourceRequest& request);
    ConfigCreateResult create_task(const CreateTaskRequest& request);
    std::vector<TaskRecord> find_enabled_tasks(const std::string& node_code) const;

private:
    INodeRepository& node_repository_;
    IConfigRepository& config_repository_;
};

}  // namespace labbridge::server
