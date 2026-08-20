#pragma once

#include "labbridge/server/postgres/sql_session.h"
#include "labbridge/server/repositories/management_query_repository.h"

namespace labbridge::server {

class PostgresManagementQueryRepository final
    : public IManagementQueryRepository {
public:
    explicit PostgresManagementQueryRepository(ISqlSession& session);

    std::vector<NodeRecord> list_nodes(
        const NodeListFilter& filter,
        const ManagementPageRequest& page) const override;
    std::optional<NodeSummaryRecord> find_node_summary(
        const std::string& node_code) const override;

    std::vector<DataSourceRecord> list_data_sources_by_node(
        const std::string& node_code,
        const EnabledListFilter& filter,
        const ManagementPageRequest& page) const override;
    std::vector<QcRuleRecord> list_qc_rules(
        const EnabledListFilter& filter,
        const ManagementPageRequest& page) const override;
    std::vector<TaskRecord> list_tasks_by_node(
        const std::string& node_code,
        const EnabledListFilter& filter,
        const ManagementPageRequest& page) const override;
    std::optional<TaskRecord> find_task(
        const std::string& task_id) const override;

    std::vector<TaskRunRecord> list_task_runs_by_node(
        const TaskRunListFilter& filter,
        const ManagementPageRequest& page) const override;
    std::optional<TaskRunSummaryRecord> find_task_run_summary(
        const std::string& task_run_id) const override;

    std::vector<RawFileRecord> list_raw_files_by_run(
        const std::string& task_run_id,
        const ManagementPageRequest& page) const override;
    std::vector<ParsedRecordRecord> list_parsed_records_by_run(
        const std::string& task_run_id,
        const ManagementPageRequest& page) const override;
    std::vector<QcResultRecord> list_qc_results_by_run(
        const QcResultListFilter& filter,
        const ManagementPageRequest& page) const override;
    std::vector<AlertRecord> list_alerts_by_node(
        const AlertListFilter& filter,
        const ManagementPageRequest& page) const override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
