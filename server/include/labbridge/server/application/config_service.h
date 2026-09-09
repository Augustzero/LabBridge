#pragma once

#include "labbridge/core/models.h"
#include "labbridge/server/repositories/config_repository.h"

#include <string>
#include <vector>

namespace labbridge::server {

struct ExecutableConfigProjection {
    std::vector<TaskRecord> tasks;
    std::vector<DataSourceRecord> data_sources;
    std::vector<TaskQcRuleBinding> task_qc_rules;
};


class ConfigService {
public:
    explicit ConfigService(IConfigRepository& config_repository);

    std::vector<TaskRecord> find_enabled_tasks(const std::string& node_code) const;
    ExecutableConfigProjection find_executable_config(
        const std::string& node_code) const;

private:
    IConfigRepository& config_repository_;
};

}  // namespace labbridge::server
