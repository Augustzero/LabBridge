#include "labbridge/server/postgres/management_query_executor.h"

#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/management_query_repository.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace labbridge::server {
namespace {

class ManagementQueryRequestScope {
public:
    ManagementQueryRequestScope(const std::string& connection_info,
                                int node_offline_after_seconds,
                                int task_run_stale_after_seconds)
        : session_(connection_info),
          repository_(session_),
          service_(repository_,
                   std::chrono::system_clock::now(),
                   node_offline_after_seconds,
                   task_run_stale_after_seconds) {}

    ManagementQueryService& service() {
        return service_;
    }

private:
    LibpqSqlSession session_;
    PostgresManagementQueryRepository repository_;
    ManagementQueryService service_;
};

}  // namespace

PostgresManagementQueryExecutor::PostgresManagementQueryExecutor(
    std::string connection_info,
    int node_offline_after_seconds,
    int task_run_stale_after_seconds)
    : connection_info_(std::move(connection_info)),
      node_offline_after_seconds_(node_offline_after_seconds),
      task_run_stale_after_seconds_(task_run_stale_after_seconds) {
    if (node_offline_after_seconds_ <= 0 ||
        task_run_stale_after_seconds_ <= 0) {
        throw std::invalid_argument(
            "management time thresholds must be positive");
    }
}

#define LABBRIDGE_MANAGEMENT_SCOPE()                                      \
    ManagementQueryRequestScope scope{connection_info_,                  \
                                      node_offline_after_seconds_,        \
                                      task_run_stale_after_seconds_}

ManagementPageResult<ManagementNode>
PostgresManagementQueryExecutor::list_nodes(
    const NodeListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_nodes(request);
}

ManagementItemResult<ManagementNodeSummary>
PostgresManagementQueryExecutor::find_node(
    const std::string& node_code) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().find_node(node_code);
}

ManagementPageResult<DataSourceRecord>
PostgresManagementQueryExecutor::list_data_sources(
    const NodeScopedListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_data_sources(request);
}

ManagementPageResult<QcRuleRecord>
PostgresManagementQueryExecutor::list_qc_rules(
    const QcRuleListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_qc_rules(request);
}

ManagementPageResult<TaskRecord>
PostgresManagementQueryExecutor::list_tasks(
    const NodeScopedListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_tasks(request);
}

ManagementPageResult<ManagementTaskRun>
PostgresManagementQueryExecutor::list_task_runs(
    const TaskRunListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_task_runs(request);
}

ManagementItemResult<ManagementTaskRunSummary>
PostgresManagementQueryExecutor::find_task_run(
    const std::string& node_code,
    const std::string& task_run_id) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().find_task_run(node_code, task_run_id);
}

ManagementPageResult<RawFileRecord>
PostgresManagementQueryExecutor::list_raw_files(
    const RunScopedListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_raw_files(request);
}

ManagementPageResult<ParsedRecordRecord>
PostgresManagementQueryExecutor::list_parsed_records(
    const RunScopedListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_parsed_records(request);
}

ManagementPageResult<QcResultRecord>
PostgresManagementQueryExecutor::list_qc_results(
    const QcResultListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_qc_results(request);
}

ManagementPageResult<AlertRecord>
PostgresManagementQueryExecutor::list_alerts(
    const AlertListRequest& request) const {
    LABBRIDGE_MANAGEMENT_SCOPE();
    return scope.service().list_alerts(request);
}

#undef LABBRIDGE_MANAGEMENT_SCOPE

}  // namespace labbridge::server
