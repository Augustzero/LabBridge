#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/repositories/management_query_repository.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace labbridge::server {

struct PageInput {
    int limit{20};
    std::optional<std::string> cursor;
};

template <typename T>
struct ManagementPage {
    std::vector<T> items;
    std::optional<std::string> next_cursor;
    bool has_more{false};
};

template <typename T>
struct ManagementPageResult {
    labbridge::core::Status status;
    ManagementPage<T> page;
};

struct ManagementNode {
    NodeRecord record;
    labbridge::core::NodeStatus effective_status{
        labbridge::core::NodeStatus::Offline};
};

struct ManagementTaskRun {
    TaskRunRecord record;
    bool stale{false};
    int stale_after_seconds{0};
};

struct ManagementNodeSummary {
    NodeSummaryRecord record;
    labbridge::core::NodeStatus effective_status{
        labbridge::core::NodeStatus::Offline};
};

struct ManagementTaskRunSummary {
    TaskRunSummaryRecord record;
    bool stale{false};
    int stale_after_seconds{0};
};

template <typename T>
struct ManagementItemResult {
    labbridge::core::Status status;
    std::optional<T> item;
};

struct NodeListRequest {
    std::optional<std::string> status;
    PageInput page;
};

struct NodeScopedListRequest {
    std::string node_code;
    std::optional<bool> enabled;
    PageInput page;
};

struct QcRuleListRequest {
    std::optional<bool> enabled;
    PageInput page;
};

struct TaskRunListRequest {
    std::string node_code;
    std::optional<std::string> task_id;
    std::optional<std::string> status;
    PageInput page;
};

struct RunScopedListRequest {
    std::string task_run_id;
    PageInput page;
};

struct QcResultListRequest {
    std::string task_run_id;
    std::optional<std::string> result;
    PageInput page;
};

struct AlertListRequest {
    std::string node_code;
    std::optional<std::string> task_run_id;
    std::optional<std::string> status;
    std::optional<std::string> severity;
    PageInput page;
};

class ManagementQueryService {
public:
    ManagementQueryService(
        IManagementQueryRepository& repository,
        std::chrono::system_clock::time_point now,
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
    IManagementQueryRepository& repository_;
    std::chrono::system_clock::time_point now_;
    int node_offline_after_seconds_;
    int task_run_stale_after_seconds_;
};

}  // namespace labbridge::server
