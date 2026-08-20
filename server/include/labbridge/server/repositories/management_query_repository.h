#pragma once

#include "labbridge/server/repositories/alert_repository.h"
#include "labbridge/server/repositories/config_repository.h"
#include "labbridge/server/repositories/node_repository.h"
#include "labbridge/server/repositories/qc_repository.h"
#include "labbridge/server/repositories/result_repository.h"
#include "labbridge/server/repositories/task_run_repository.h"

#include <optional>
#include <string>
#include <vector>

namespace labbridge::server {

struct ManagementPageRequest {
    int fetch_limit{21};
    std::optional<std::string> cursor;
};

struct NodeListFilter {
    std::optional<labbridge::core::NodeStatus> effective_status;
    std::string now;
    int offline_after_seconds{60};
};

struct EnabledListFilter {
    std::optional<bool> enabled;
};

struct TaskRunListFilter {
    std::string node_code;
    std::optional<std::string> task_id;
    std::optional<labbridge::core::TaskRunStatus> status;
};

struct QcResultListFilter {
    std::string task_run_id;
    std::optional<std::string> result;
};

struct AlertListFilter {
    std::string node_code;
    std::optional<std::string> task_run_id;
    std::optional<std::string> status;
    std::optional<std::string> severity;
};

struct NodeSummaryRecord {
    NodeRecord node;
    int enabled_task_count{0};
    int disabled_task_count{0};
    int open_alert_count{0};
    std::optional<TaskRunRecord> latest_task_run;
};

struct TaskRunSummaryRecord {
    TaskRunRecord task_run;
    int raw_file_count{0};
    int parsed_record_count{0};
    int qc_result_count{0};
    int alert_count{0};
};

class IManagementQueryRepository {
public:
    virtual ~IManagementQueryRepository() = default;

    virtual std::vector<NodeRecord> list_nodes(
        const NodeListFilter& filter,
        const ManagementPageRequest& page) const = 0;
    virtual std::optional<NodeSummaryRecord> find_node_summary(
        const std::string& node_code) const = 0;

    virtual std::vector<DataSourceRecord> list_data_sources_by_node(
        const std::string& node_code,
        const EnabledListFilter& filter,
        const ManagementPageRequest& page) const = 0;
    virtual std::vector<QcRuleRecord> list_qc_rules(
        const EnabledListFilter& filter,
        const ManagementPageRequest& page) const = 0;
    virtual std::vector<TaskRecord> list_tasks_by_node(
        const std::string& node_code,
        const EnabledListFilter& filter,
        const ManagementPageRequest& page) const = 0;
    virtual std::optional<TaskRecord> find_task(
        const std::string& task_id) const = 0;

    virtual std::vector<TaskRunRecord> list_task_runs_by_node(
        const TaskRunListFilter& filter,
        const ManagementPageRequest& page) const = 0;
    virtual std::optional<TaskRunSummaryRecord> find_task_run_summary(
        const std::string& task_run_id) const = 0;

    virtual std::vector<RawFileRecord> list_raw_files_by_run(
        const std::string& task_run_id,
        const ManagementPageRequest& page) const = 0;
    virtual std::vector<ParsedRecordRecord> list_parsed_records_by_run(
        const std::string& task_run_id,
        const ManagementPageRequest& page) const = 0;
    virtual std::vector<QcResultRecord> list_qc_results_by_run(
        const QcResultListFilter& filter,
        const ManagementPageRequest& page) const = 0;
    virtual std::vector<AlertRecord> list_alerts_by_node(
        const AlertListFilter& filter,
        const ManagementPageRequest& page) const = 0;
};

}  // namespace labbridge::server
