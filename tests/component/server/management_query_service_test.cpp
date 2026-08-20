#include "labbridge/server/application/management_query_service.h"

#include <gtest/gtest.h>

namespace {

using namespace labbridge::server;

class FakeManagementQueryRepository final
    : public IManagementQueryRepository {
public:
    std::vector<NodeRecord> list_nodes(
        const NodeListFilter& filter,
        const ManagementPageRequest& page) const override {
        last_node_filter = filter;
        last_page = page;
        return nodes;
    }

    std::optional<NodeSummaryRecord> find_node_summary(
        const std::string& node_code) const override {
        if (node_summary.has_value() &&
            node_summary->node.info.node_code == node_code) {
            return node_summary;
        }
        return std::nullopt;
    }

    std::vector<DataSourceRecord> list_data_sources_by_node(
        const std::string&,
        const EnabledListFilter&,
        const ManagementPageRequest&) const override {
        return data_sources;
    }

    std::vector<QcRuleRecord> list_qc_rules(
        const EnabledListFilter&,
        const ManagementPageRequest&) const override {
        return qc_rules;
    }

    std::vector<TaskRecord> list_tasks_by_node(
        const std::string&,
        const EnabledListFilter&,
        const ManagementPageRequest&) const override {
        return tasks;
    }

    std::optional<TaskRecord> find_task(
        const std::string& task_id) const override {
        for (const auto& task : tasks) {
            if (task.id == task_id) {
                return task;
            }
        }
        return std::nullopt;
    }

    std::vector<TaskRunRecord> list_task_runs_by_node(
        const TaskRunListFilter&,
        const ManagementPageRequest&) const override {
        return task_runs;
    }

    std::optional<TaskRunSummaryRecord> find_task_run_summary(
        const std::string& task_run_id) const override {
        if (run_summary.has_value() &&
            run_summary->task_run.id == task_run_id) {
            return run_summary;
        }
        return std::nullopt;
    }

    std::vector<RawFileRecord> list_raw_files_by_run(
        const std::string&,
        const ManagementPageRequest&) const override {
        return raw_files;
    }

    std::vector<ParsedRecordRecord> list_parsed_records_by_run(
        const std::string&,
        const ManagementPageRequest&) const override {
        return parsed_records;
    }

    std::vector<QcResultRecord> list_qc_results_by_run(
        const QcResultListFilter&,
        const ManagementPageRequest&) const override {
        return qc_results;
    }

    std::vector<AlertRecord> list_alerts_by_node(
        const AlertListFilter&,
        const ManagementPageRequest&) const override {
        return alerts;
    }

    mutable NodeListFilter last_node_filter;
    mutable ManagementPageRequest last_page;
    std::vector<NodeRecord> nodes;
    std::optional<NodeSummaryRecord> node_summary;
    std::vector<DataSourceRecord> data_sources;
    std::vector<QcRuleRecord> qc_rules;
    std::vector<TaskRecord> tasks;
    std::vector<TaskRunRecord> task_runs;
    std::optional<TaskRunSummaryRecord> run_summary;
    std::vector<RawFileRecord> raw_files;
    std::vector<ParsedRecordRecord> parsed_records;
    std::vector<QcResultRecord> qc_results;
    std::vector<AlertRecord> alerts;
};

std::chrono::system_clock::time_point fixed_now() {
    return std::chrono::system_clock::time_point{
        std::chrono::seconds{1787097600}};
}

NodeRecord node(
    std::string id,
    std::string heartbeat,
    labbridge::core::NodeStatus stored =
        labbridge::core::NodeStatus::Online) {
    NodeRecord record;
    record.id = std::move(id);
    record.info.node_code = "node-a";
    record.info.name = "Node A";
    record.status = stored;
    record.last_heartbeat_at = std::move(heartbeat);
    return record;
}

TEST(ManagementQueryServiceTest,
     DerivesEffectiveStatusAndReturnsStableNextCursor) {
    FakeManagementQueryRepository repository;
    repository.nodes = {
        node("5", "2026-08-19T00:00:00Z"),
        node("4", "2026-08-18T23:59:59Z"),
        node("3", ""),
    };
    ManagementQueryService service{
        repository, fixed_now(), 0, 3600};

    const auto result = service.list_nodes(
        {std::string{"online"}, {2, std::nullopt}});

    ASSERT_TRUE(result.status.ok);
    ASSERT_EQ(result.page.items.size(), 2U);
    EXPECT_EQ(
        result.page.items[0].effective_status,
        labbridge::core::NodeStatus::Online);
    EXPECT_EQ(
        result.page.items[1].effective_status,
        labbridge::core::NodeStatus::Offline);
    EXPECT_TRUE(result.page.has_more);
    EXPECT_EQ(result.page.next_cursor, std::optional<std::string>{"4"});
    EXPECT_EQ(repository.last_page.fetch_limit, 3);
    EXPECT_EQ(repository.last_node_filter.now,
              "2026-08-19T00:00:00Z");
}

TEST(ManagementQueryServiceTest,
     RejectsInvalidPageCursorAndEnumBeforeRepository) {
    FakeManagementQueryRepository repository;
    ManagementQueryService service{
        repository, fixed_now(), 60, 3600};

    EXPECT_FALSE(service.list_nodes(
        {std::nullopt, {0, std::nullopt}}).status.ok);
    EXPECT_FALSE(service.list_nodes(
        {std::nullopt, {101, std::nullopt}}).status.ok);
    EXPECT_FALSE(service.list_nodes(
        {std::nullopt, {20, std::string{"0"}}}).status.ok);
    EXPECT_FALSE(service.list_nodes(
        {std::string{"ONLINE"}, {20, std::nullopt}}).status.ok);
    EXPECT_TRUE(repository.nodes.empty());
}

TEST(ManagementQueryServiceTest,
     ManagementConfigurationIncludesDisabledRecordsAndRuleOrder) {
    FakeManagementQueryRepository repository;
    repository.node_summary = NodeSummaryRecord{
        node("1", "2026-08-19T00:00:00Z")};
    DataSourceRecord source;
    source.id = "10";
    source.node_code = "node-a";
    source.enabled = false;
    repository.data_sources = {source};

    TaskRecord task;
    task.id = "20";
    task.node_code = "node-a";
    task.enabled = false;
    task.qc_rule_ids = {"31", "32"};
    repository.tasks = {task};

    ManagementQueryService service{
        repository, fixed_now(), 60, 3600};

    const auto sources = service.list_data_sources(
        {"node-a", std::nullopt, {20, std::nullopt}});
    const auto tasks = service.list_tasks(
        {"node-a", std::nullopt, {20, std::nullopt}});

    ASSERT_TRUE(sources.status.ok);
    ASSERT_EQ(sources.page.items.size(), 1U);
    EXPECT_FALSE(sources.page.items.front().enabled);
    ASSERT_TRUE(tasks.status.ok);
    EXPECT_EQ(tasks.page.items.front().qc_rule_ids,
              (std::vector<std::string>{"31", "32"}));
}

TEST(ManagementQueryServiceTest,
     StaleIsReadOnlyAndUsesStrictThresholdBoundary) {
    FakeManagementQueryRepository repository;
    repository.node_summary = NodeSummaryRecord{
        node("1", "2026-08-19T00:00:00Z")};

    TaskRunRecord boundary;
    boundary.id = "12";
    boundary.task_id = "20";
    boundary.node_code = "node-a";
    boundary.status = labbridge::core::TaskRunStatus::Running;
    boundary.started_at = "2026-08-18T23:00:00Z";

    TaskRunRecord stale = boundary;
    stale.id = "11";
    stale.started_at = "2026-08-18T22:59:59Z";
    repository.task_runs = {boundary, stale};

    ManagementQueryService service{
        repository, fixed_now(), 60, 3600};
    const auto result = service.list_task_runs(
        {"node-a", std::nullopt, std::string{"running"},
         {20, std::nullopt}});

    ASSERT_TRUE(result.status.ok);
    ASSERT_EQ(result.page.items.size(), 2U);
    EXPECT_FALSE(result.page.items[0].stale);
    EXPECT_TRUE(result.page.items[1].stale);
    EXPECT_EQ(repository.task_runs[1].status,
              labbridge::core::TaskRunStatus::Running);
}

TEST(ManagementQueryServiceTest,
     EvidenceRequiresExistingRunAndKeepsRelationsVisible) {
    FakeManagementQueryRepository repository;
    TaskRunSummaryRecord summary;
    summary.task_run.id = "50";
    summary.task_run.node_code = "node-a";
    repository.run_summary = summary;

    QcResultRecord qc;
    qc.id = "80";
    qc.task_run_id = "50";
    qc.parsed_record_id = "70";
    qc.qc_rule_id = "30";
    qc.result = "failed";
    repository.qc_results = {qc};

    ManagementQueryService service{
        repository, fixed_now(), 60, 3600};

    const auto missing = service.list_raw_files(
        {"49", {20, std::nullopt}});
    EXPECT_EQ(missing.status.code,
              labbridge::core::StatusCode::NotFound);

    const auto results = service.list_qc_results(
        {"50", std::string{"failed"}, {20, std::nullopt}});
    ASSERT_TRUE(results.status.ok);
    ASSERT_EQ(results.page.items.size(), 1U);
    EXPECT_EQ(results.page.items.front().parsed_record_id, "70");
    EXPECT_EQ(results.page.items.front().qc_rule_id, "30");
}

}  // namespace
