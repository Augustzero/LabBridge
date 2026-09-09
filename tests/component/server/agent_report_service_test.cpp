#include "support/server/in_memory_repositories.h"
#include "support/server/test_config_seed.h"
#include "labbridge/core/version.h"
#include "labbridge/server/application/agent_report_service.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/application/task_run_service.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

bool contains_id(const std::vector<std::string>& ids, const std::string& id) {
    for (const auto& current : ids) {
        if (current == id) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(AgentReportServiceTest, ProcessesManifestAndTaskRunReport) {
    labbridge::server::InMemoryNodeRepository node_repository;
    labbridge::server::InMemoryConfigRepository config_repository;
    labbridge::server::InMemoryTaskRunRepository task_run_repository;
    labbridge::server::InMemoryResultRepository result_repository;
    labbridge::server::InMemoryQcRepository qc_repository;
    labbridge::server::InMemoryAlertRepository alert_repository;
    labbridge::server::InMemoryAgentReportReceiptRepository receipt_repository;

    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{config_repository};
    labbridge::server::TaskRunService task_run_service{config_repository, task_run_repository};
    labbridge::server::ResultService result_service{task_run_repository, result_repository};
    labbridge::server::QcService qc_service{qc_repository};
    labbridge::server::AlertService alert_service{
        task_run_repository,
        result_repository,
        qc_repository,
        alert_repository};
    labbridge::server::AgentReportService agent_report_service{
        task_run_service,
        result_service,
        qc_service,
        alert_service,
        receipt_repository};

    const std::string node_code = "lab-node-report-016";
    const std::string other_node_code = "lab-node-report-016-other";

    ASSERT_TRUE(node_service.register_node({node_code, "report-node", labbridge::core::kVersion}).ok);
    ASSERT_TRUE(node_service.register_node({other_node_code, "report-other-node", labbridge::core::kVersion}).ok);
    ASSERT_TRUE(node_service.accept_heartbeat({
               node_code,
               labbridge::core::kVersion,
               "2026-06-02T02:00:00Z",
           }).ok);

    const auto data_source_id =
        labbridge::server::test_support::create_local_csv_data_source(
            config_repository, node_code, "phase16 local csv dir");
    const auto task_id = labbridge::server::test_support::create_csv_task(
        config_repository, node_code, data_source_id,
        "phase16 agent reported csv");

    const auto started = task_run_service.start({
        node_code,
        task_id,
        "2026-06-02T02:01:00Z",
        "agent_report",
    });
    ASSERT_TRUE(started.status.ok);

    const auto wrong_node_manifest = agent_report_service.accept_raw_file_manifest({
        started.id,
        other_node_code,
        "phase16-wrong-node-manifest",
        {},
    });
    ASSERT_TRUE(!wrong_node_manifest.status.ok);

    const auto manifest = agent_report_service.accept_raw_file_manifest({
        started.id,
        node_code,
        "phase16-manifest",
        {
            {
                "phase16_observation.csv",
                "phase16-hash-archived",
                "/archive/phase16/phase16_observation.csv",
                256,
                "2026-06-02T01:59:00Z",
                "archived",
            },
        },
    });
    ASSERT_TRUE(manifest.status.ok);
    ASSERT_TRUE(manifest.raw_file_ids.size() == 1);

    const auto rule = qc_service.create_rule({
        "phase16 reported temperature range",
        "range_check",
        "{}",
        true,
    });
    ASSERT_TRUE(rule.status.ok);

    const auto invalid_status_report = agent_report_service.accept_task_run_report({
        started.id,
        node_code,
        "phase16-invalid-status-report",
        labbridge::core::TaskRunStatus::Running,
        "2026-06-02T02:03:00Z",
        1,
        0,
        1,
        "invalid finish status",
        {},
    });
    ASSERT_TRUE(!invalid_status_report.status.ok);

    const auto unknown_rule_report = agent_report_service.accept_task_run_report({
        started.id,
        node_code,
        "phase16-unknown-rule-report",
        labbridge::core::TaskRunStatus::Failed,
        "2026-06-02T02:03:00Z",
        1,
        0,
        1,
        "qc failed",
        {
            {
                manifest.raw_file_ids.front(),
                {
                    "station-a",
                    "device-a",
                    "2026-06-02T02:00:00Z",
                    "[48.5]",
                },
                "parsed",
                {
                    {"999999", "failed", "failed", "unknown rule"},
                },
            },
        },
    });
    ASSERT_TRUE(!unknown_rule_report.status.ok);
    ASSERT_TRUE(unknown_rule_report.status.code ==
               labbridge::core::StatusCode::NotFound);

    // 空 ID 属于参数缺失（400），不得因先查库而退化为 404。
    const auto empty_rule_report = agent_report_service.accept_task_run_report({
        started.id,
        node_code,
        "phase16-empty-rule-report",
        labbridge::core::TaskRunStatus::Failed,
        "2026-06-02T02:03:00Z",
        1,
        0,
        1,
        "qc failed",
        {
            {
                manifest.raw_file_ids.front(),
                {
                    "station-a",
                    "device-a",
                    "2026-06-02T02:00:00Z",
                    "[48.5]",
                },
                "parsed",
                {
                    {"", "failed", "failed", "empty rule id"},
                },
            },
        },
    });
    ASSERT_TRUE(!empty_rule_report.status.ok);
    ASSERT_TRUE(empty_rule_report.status.code ==
                labbridge::core::StatusCode::InvalidArgument);

    const auto empty_raw_file_report =
        agent_report_service.accept_task_run_report({
            started.id,
            node_code,
            "phase16-empty-raw-file-report",
            labbridge::core::TaskRunStatus::Failed,
            "2026-06-02T02:03:00Z",
            1,
            0,
            1,
            "qc failed",
            {
                {
                    "",
                    {
                        "station-a",
                        "device-a",
                        "2026-06-02T02:00:00Z",
                        "[48.5]",
                    },
                    "parsed",
                    {},
                },
            },
        });
    ASSERT_TRUE(!empty_raw_file_report.status.ok);
    ASSERT_TRUE(empty_raw_file_report.status.code ==
                labbridge::core::StatusCode::InvalidArgument);

    const auto report = agent_report_service.accept_task_run_report({
        started.id,
        node_code,
        "phase16-report",
        labbridge::core::TaskRunStatus::Failed,
        "2026-06-02T02:03:00Z",
        1,
        0,
        1,
        "qc failed",
        {
            {
                manifest.raw_file_ids.front(),
                {
                    "station-a",
                    "device-a",
                    "2026-06-02T02:00:00Z",
                    "[48.5]",
                },
                "parsed",
                {
                    {rule.id, "pass", "passed", "humidity is in range"},
                    {rule.id, "failed", "failed", "temperature is outside configured range"},
                },
            },
        },
    });
    ASSERT_TRUE(report.status.ok);
    ASSERT_TRUE(report.parsed_record_ids.size() == 1);
    ASSERT_TRUE(report.qc_result_ids.size() == 2);
    ASSERT_TRUE(report.alert_ids.size() == 1);

    // 逐对象回验：run 终态、raw file、parsed record、qc result、alert。
    const auto finished = task_run_service.find_run(started.id);
    ASSERT_TRUE(finished.has_value());
    ASSERT_TRUE(finished->status == labbridge::core::TaskRunStatus::Failed);
    ASSERT_TRUE(finished->items_total == 1);
    ASSERT_TRUE(finished->items_failed == 1);

    const auto raw_file = result_repository.find_raw_file(manifest.raw_file_ids.front());
    ASSERT_TRUE(raw_file.has_value());
    ASSERT_TRUE(raw_file->node_code == node_code);
    ASSERT_TRUE(raw_file->storage_path == "/archive/phase16/phase16_observation.csv");
    ASSERT_TRUE(raw_file->ingest_status == "archived");

    const auto parsed_record =
        result_repository.find_parsed_record(report.parsed_record_ids.front());
    ASSERT_TRUE(parsed_record.has_value());
    ASSERT_TRUE(parsed_record->raw_file_id == manifest.raw_file_ids.front());

    for (const auto& qc_result_id : report.qc_result_ids) {
        const auto qc_result = qc_repository.find_result(qc_result_id);
        ASSERT_TRUE(qc_result.has_value());
        ASSERT_TRUE(qc_result->parsed_record_id == parsed_record->id);
    }

    const auto node_alerts = alert_repository.find_by_node(node_code);
    ASSERT_TRUE(node_alerts.size() == 1);
    ASSERT_TRUE(contains_id(
        std::vector<std::string>{node_alerts.front().id},
        report.alert_ids.front()));
}
