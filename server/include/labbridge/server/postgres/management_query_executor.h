#pragma once

#include "labbridge/server/application/management_query_service.h"

#include <string>

namespace labbridge::server {

class PostgresManagementQueryExecutor {
public:
    PostgresManagementQueryExecutor(std::string connection_info,
                                    int node_offline_after_seconds,
                                    int task_run_stale_after_seconds);

    ManagementPageResult<ManagementNode> list_nodes(
        const NodeListRequest& request) const;
    ManagementItemResult<ManagementNodeSummary> find_node(
        const std::string& node_code) const;
    ManagementPageResult<DataSourceRecord> list_data_sources(
        const NodeScopedListRequest& request) const;
    ManagementPageResult<QcRuleRecord> list_qc_rules(
        const QcRuleListRequest& request) const;
    ManagementPageResult<TaskRecord> list_tasks(
        const NodeScopedListRequest& request) const;
    ManagementPageResult<ManagementTaskRun> list_task_runs(
        const TaskRunListRequest& request) const;
    ManagementItemResult<ManagementTaskRunSummary> find_task_run(
        const std::string& node_code,
        const std::string& task_run_id) const;
    ManagementPageResult<RawFileRecord> list_raw_files(
        const RunScopedListRequest& request) const;
    ManagementPageResult<ParsedRecordRecord> list_parsed_records(
        const RunScopedListRequest& request) const;
    ManagementPageResult<QcResultRecord> list_qc_results(
        const QcResultListRequest& request) const;
    ManagementPageResult<AlertRecord> list_alerts(
        const AlertListRequest& request) const;

private:
    std::string connection_info_;
    int node_offline_after_seconds_;
    int task_run_stale_after_seconds_;
};

}  // namespace labbridge::server
