#include "labbridge/server/postgres/management_command_executor.h"

#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/postgres/alert_repository.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/sql_transaction.h"
#include "labbridge/server/postgres/task_run_repository.h"

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
          task_run_repository_(session_),
          result_repository_(session_),
          alert_repository_(session_),
          service_(node_repository_, config_repository_, qc_repository_,
                   task_run_repository_),
          alert_service_(
              task_run_repository_, result_repository_, qc_repository_,
              alert_repository_),
          task_run_service_(config_repository_, task_run_repository_,
                            node_repository_, qc_repository_,
                            result_repository_) {}

    ManagementCommandService& service() {
        return service_;
    }

    AlertService& alert_service() {
        return alert_service_;
    }

    TaskRunService& task_run_service() {
        return task_run_service_;
    }

    void commit_if_successful(const labbridge::core::Status& status) {
        // 业务拒绝和数据库异常都必须离开作用域触发回滚。
        if (status.ok) {
            transaction_.commit();
        }
    }

private:
    LibpqSqlSession session_;
    SqlTransaction transaction_;
    PostgresNodeRepository node_repository_;
    PostgresConfigRepository config_repository_;
    PostgresQcRepository qc_repository_;
    PostgresTaskRunRepository task_run_repository_;
    PostgresResultRepository result_repository_;
    PostgresAlertRepository alert_repository_;
    ManagementCommandService service_;
    AlertService alert_service_;
    TaskRunService task_run_service_;
};

template <typename Result, typename Operation>
Result execute_command(const std::string& connection_info,
                       Operation operation) {
    ManagementCommandRequestScope scope{connection_info};
    auto result = operation(scope);
    scope.commit_if_successful(result.status);
    return result;
}

}  // namespace

PostgresManagementCommandExecutor::PostgresManagementCommandExecutor(
    std::string connection_info)
    : connection_info_(std::move(connection_info)) {}

ManagementCommandResult PostgresManagementCommandExecutor::create_data_source(
    const ManagementDataSourceCreateRequest& request) const {
    return execute_command<ManagementCommandResult>(
        connection_info_,
        [&request](ManagementCommandRequestScope& scope) {
            return scope.service().create_data_source(request);
        });
}

ManagementCommandResult PostgresManagementCommandExecutor::create_qc_rule(
    const ManagementQcRuleCreateRequest& request) const {
    return execute_command<ManagementCommandResult>(
        connection_info_,
        [&request](ManagementCommandRequestScope& scope) {
            return scope.service().create_qc_rule(request);
        });
}

ManagementCommandResult PostgresManagementCommandExecutor::create_task(
    const ManagementTaskCreateRequest& request) const {
    return execute_command<ManagementCommandResult>(
        connection_info_,
        [&request](ManagementCommandRequestScope& scope) {
            return scope.service().create_task(request);
        });
}

ManagementCommandResult PostgresManagementCommandExecutor::set_task_enabled(
    const std::string& task_id,
    bool enabled) const {
    return execute_command<ManagementCommandResult>(
        connection_info_,
        [&task_id, enabled](ManagementCommandRequestScope& scope) {
            return scope.service().set_task_enabled(task_id, enabled);
        });
}

ManualTaskRunResult PostgresManagementCommandExecutor::trigger_task_run(
    const std::string& task_id,
    const std::string& idempotency_key) const {
    return execute_command<ManualTaskRunResult>(
        connection_info_,
        [&task_id, &idempotency_key](ManagementCommandRequestScope& scope) {
            return scope.task_run_service().request_manual_run(
                task_id, idempotency_key);
        });
}

ManualTaskRunResult PostgresManagementCommandExecutor::retry_task_run(
    const std::string& task_run_id,
    const std::string& idempotency_key) const {
    return execute_command<ManualTaskRunResult>(
        connection_info_,
        [&task_run_id, &idempotency_key](ManagementCommandRequestScope& scope) {
            return scope.task_run_service().request_retry_run(
                task_run_id, idempotency_key);
        });
}

AlertDispositionResult PostgresManagementCommandExecutor::acknowledge_alert(
    const std::string& alert_id) const {
    return execute_command<AlertDispositionResult>(
        connection_info_,
        [&alert_id](ManagementCommandRequestScope& scope) {
            return scope.alert_service().acknowledge_alert(alert_id);
        });
}

AlertDispositionResult PostgresManagementCommandExecutor::close_alert(
    const std::string& alert_id) const {
    return execute_command<AlertDispositionResult>(
        connection_info_,
        [&alert_id](ManagementCommandRequestScope& scope) {
            return scope.alert_service().close_alert(alert_id);
        });
}

}  // namespace labbridge::server
