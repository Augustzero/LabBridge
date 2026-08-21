#pragma once

#include "labbridge/server/application/management_command_service.h"

#include <string>

namespace labbridge::server {

class PostgresManagementCommandExecutor {
public:
    explicit PostgresManagementCommandExecutor(std::string connection_info);

    ManagementCommandResult create_data_source(
        const ManagementDataSourceCreateRequest& request) const;
    ManagementCommandResult create_qc_rule(
        const ManagementQcRuleCreateRequest& request) const;
    ManagementCommandResult create_task(
        const ManagementTaskCreateRequest& request) const;
    ManagementCommandResult set_task_enabled(const std::string& task_id,
                                             bool enabled) const;

private:
    std::string connection_info_;
};

}  // namespace labbridge::server
