#include "labbridge/server/postgres/management_command_executor.h"

#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/sql_transaction.h"

#include <utility>

namespace labbridge::server {
namespace {

class ManagementCommandRequestScope {
public:
    explicit ManagementCommandRequestScope(const std::string& connection_info)
        : session_(connection_info),
          transaction_(session_),
          node_repository_(session_),
          config_repository_(session_),
          qc_repository_(session_),
          service_(node_repository_, config_repository_, qc_repository_) {}

    ManagementCommandService& service() {
        return service_;
    }

    void commit_if_successful(const ManagementCommandResult& result) {
        // 业务拒绝和数据库异常都必须离开作用域触发回滚。
        if (result.status.ok) {
            transaction_.commit();
        }
    }

private:
    LibpqSqlSession session_;
    SqlTransaction transaction_;
    PostgresNodeRepository node_repository_;
    PostgresConfigRepository config_repository_;
    PostgresQcRepository qc_repository_;
    ManagementCommandService service_;
};

template <typename Operation>
ManagementCommandResult execute_command(
    const std::string& connection_info,
    Operation operation) {
    ManagementCommandRequestScope scope{connection_info};
    const auto result = operation(scope.service());
    scope.commit_if_successful(result);
    return result;
}

}  // namespace

PostgresManagementCommandExecutor::PostgresManagementCommandExecutor(
    std::string connection_info)
    : connection_info_(std::move(connection_info)) {}

ManagementCommandResult PostgresManagementCommandExecutor::create_data_source(
    const ManagementDataSourceCreateRequest& request) const {
    return execute_command(
        connection_info_,
        [&request](ManagementCommandService& service) {
            return service.create_data_source(request);
        });
}

ManagementCommandResult PostgresManagementCommandExecutor::create_qc_rule(
    const ManagementQcRuleCreateRequest& request) const {
    return execute_command(
        connection_info_,
        [&request](ManagementCommandService& service) {
            return service.create_qc_rule(request);
        });
}

ManagementCommandResult PostgresManagementCommandExecutor::create_task(
    const ManagementTaskCreateRequest& request) const {
    return execute_command(
        connection_info_,
        [&request](ManagementCommandService& service) {
            return service.create_task(request);
        });
}

ManagementCommandResult PostgresManagementCommandExecutor::set_task_enabled(
    const std::string& task_id,
    bool enabled) const {
    return execute_command(
        connection_info_,
        [&task_id, enabled](ManagementCommandService& service) {
            return service.set_task_enabled(task_id, enabled);
        });
}

}  // namespace labbridge::server
