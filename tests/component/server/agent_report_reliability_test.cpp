#include "support/server/in_memory_repositories.h"
#include "labbridge/core/version.h"
#include "labbridge/server/application/agent_report_service.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/postgres/sql_transaction.h"
#include "labbridge/server/application/task_run_service.h"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace {

class RecordingSqlSession final : public labbridge::server::ISqlSession {
public:
    void execute(const std::string& sql,
                 const labbridge::server::SqlParams&) override {
        commands.push_back(sql);
    }

    std::optional<labbridge::server::SqlRow> query_one(
        const std::string&,
        const labbridge::server::SqlParams&) override {
        return std::nullopt;
    }

    std::vector<labbridge::server::SqlRow> query_all(
        const std::string&,
        const labbridge::server::SqlParams&) override {
        return {};
    }

    std::vector<std::string> commands;
};

}  // namespace

TEST(AgentReportReliabilityTest, PreservesTransactionAndIdempotencyBehavior) {
    {
        RecordingSqlSession session;
        {
            labbridge::server::SqlTransaction transaction{session};
        }
        ASSERT_TRUE((session.commands == std::vector<std::string>{"BEGIN", "ROLLBACK"}));
    }
    {
        RecordingSqlSession session;
        {
            labbridge::server::SqlTransaction transaction{session};
            transaction.commit();
        }
        ASSERT_TRUE((session.commands == std::vector<std::string>{"BEGIN", "COMMIT"}));
    }

    labbridge::server::InMemoryNodeRepository node_repository;
    labbridge::server::InMemoryConfigRepository config_repository;
    labbridge::server::InMemoryTaskRunRepository task_run_repository;
    labbridge::server::InMemoryResultRepository result_repository;
    labbridge::server::InMemoryQcRepository qc_repository;
    labbridge::server::InMemoryAlertRepository alert_repository;
    labbridge::server::InMemoryAgentReportReceiptRepository receipt_repository;

    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{node_repository, config_repository};
    labbridge::server::TaskRunService task_run_service{
        config_repository,
        task_run_repository};
    labbridge::server::ResultService result_service{
        task_run_repository,
        result_repository};
    labbridge::server::QcService qc_service{result_repository, qc_repository};
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

    const std::string node_code = "lab-node-report-018";
    ASSERT_TRUE(node_service.register_node(
               {node_code, "phase18-report-node", labbridge::core::kVersion})
               .ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase18 local csv dir",
        "{}",
        true,
    });
    ASSERT_TRUE(data_source.status.ok);

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase18 reliable report",
        "collect_parse_qc",
        "* * * * *",
        "csv_observation",
        "basic",
        true,
    });
    ASSERT_TRUE(task.status.ok);

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-07-17 10:01:00+08",
        "agent_report",
    });
    ASSERT_TRUE(started.status.ok);

    labbridge::server::RawFileManifestRequest manifest_request;
    manifest_request.task_run_id = started.id;
    manifest_request.node_code = node_code;
    manifest_request.idempotency_key = "phase18-manifest";
    manifest_request.files.push_back({
        "phase18_observation.csv",
        "phase18-hash",
        "/archive/phase18/phase18_observation.csv",
        512,
        "2026-07-17 10:00:00+08",
        {},
    });

    auto missing_key = manifest_request;
    missing_key.idempotency_key.clear();
    ASSERT_TRUE(!agent_report_service.accept_raw_file_manifest(missing_key).status.ok);

    const auto manifest = agent_report_service.accept_raw_file_manifest(manifest_request);
    ASSERT_TRUE(manifest.status.ok);
    ASSERT_TRUE(!manifest.replayed);
    ASSERT_TRUE(manifest.raw_file_ids.size() == 1);

    auto normalized_manifest = manifest_request;
    normalized_manifest.files.front().ingest_status = "collected";
    const auto manifest_replay =
        agent_report_service.accept_raw_file_manifest(normalized_manifest);
    ASSERT_TRUE(manifest_replay.status.ok);
    ASSERT_TRUE(manifest_replay.replayed);
    ASSERT_TRUE(manifest_replay.raw_file_ids == manifest.raw_file_ids);
    ASSERT_TRUE(result_service.find_raw_files(started.id).size() == 1);

    auto manifest_conflict = manifest_request;
    manifest_conflict.files.front().storage_path = "/archive/phase18/other.csv";
    const auto conflicting_manifest =
        agent_report_service.accept_raw_file_manifest(manifest_conflict);
    ASSERT_TRUE(!conflicting_manifest.status.ok);
    ASSERT_TRUE(conflicting_manifest.status.code == labbridge::core::StatusCode::Conflict);

    const auto rule = qc_service.create_rule({
        "phase18 temperature range",
        "range_check",
        "{}",
        true,
    });
    ASSERT_TRUE(rule.status.ok);

    labbridge::server::TaskRunReportRequest report_request;
    report_request.task_run_id = started.id;
    report_request.node_code = node_code;
    report_request.idempotency_key = "phase18-report";
    report_request.status = labbridge::core::TaskRunStatus::Failed;
    report_request.finished_at = "2026-07-17 10:03:00+08";
    report_request.items_total = 1;
    report_request.items_failed = 1;
    report_request.error_summary = "qc failed";
    report_request.parsed_records.push_back({
        manifest.raw_file_ids.front(),
        {
            "station-a",
            "device-a",
            "2026-07-17 10:00:00+08",
            "{\"temperature\":48.5}",
        },
        {},
        {
            {
                rule.id,
                "failed",
                "failed",
                "temperature is outside configured range",
            },
        },
    });

    const auto report = agent_report_service.accept_task_run_report(report_request);
    ASSERT_TRUE(report.status.ok);
    ASSERT_TRUE(!report.replayed);
    ASSERT_TRUE(report.parsed_record_ids.size() == 1);
    ASSERT_TRUE(report.qc_result_ids.size() == 1);
    ASSERT_TRUE(report.alert_ids.size() == 1);

    auto normalized_report = report_request;
    normalized_report.parsed_records.front().parse_status = "parsed";
    const auto report_replay =
        agent_report_service.accept_task_run_report(normalized_report);
    ASSERT_TRUE(report_replay.status.ok);
    ASSERT_TRUE(report_replay.replayed);
    ASSERT_TRUE(report_replay.parsed_record_ids == report.parsed_record_ids);
    ASSERT_TRUE(report_replay.qc_result_ids == report.qc_result_ids);
    ASSERT_TRUE(report_replay.alert_ids == report.alert_ids);
    ASSERT_TRUE(result_service.find_parsed_records(started.id).size() == 1);

    auto report_conflict = report_request;
    report_conflict.error_summary = "different request";
    const auto conflicting_report =
        agent_report_service.accept_task_run_report(report_conflict);
    ASSERT_TRUE(!conflicting_report.status.ok);
    ASSERT_TRUE(conflicting_report.status.code == labbridge::core::StatusCode::Conflict);

    auto second_report = report_request;
    second_report.idempotency_key = "phase18-second-report";
    const auto second_report_result =
        agent_report_service.accept_task_run_report(second_report);
    ASSERT_TRUE(!second_report_result.status.ok);
    ASSERT_TRUE(second_report_result.status.code == labbridge::core::StatusCode::Conflict);

    const auto finished = task_run_service.find_run(started.id);
    ASSERT_TRUE(finished.has_value());
    ASSERT_TRUE(finished->status == labbridge::core::TaskRunStatus::Failed);
}
