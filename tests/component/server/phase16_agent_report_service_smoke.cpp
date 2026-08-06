#include "support/server/in_memory_repositories.h"
#include "labbridge/core/version.h"
#include "labbridge/server/application/agent_report_service.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/query_service.h"
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

bool contains_qc_result(const std::vector<labbridge::server::QcResultRecord>& results,
                        const std::string& qc_result_id) {
    for (const auto& result : results) {
        if (result.id == qc_result_id) {
            return true;
        }
    }
    return false;
}

bool contains_alert(const std::vector<labbridge::server::AlertRecord>& alerts,
                    const std::string& alert_id) {
    for (const auto& alert : alerts) {
        if (alert.id == alert_id) {
            return true;
        }
    }
    return false;
}

const labbridge::server::RawFileRecord* find_raw_file(
    const std::vector<labbridge::server::RawFileRecord>& raw_files,
    const std::string& raw_file_id) {
    for (const auto& raw_file : raw_files) {
        if (raw_file.id == raw_file_id) {
            return &raw_file;
        }
    }
    return nullptr;
}

}  // namespace

TEST(Phase016AgentReportServiceTest, ProcessesManifestAndTaskRunReport) {
    labbridge::server::InMemoryNodeRepository node_repository;
    labbridge::server::InMemoryConfigRepository config_repository;
    labbridge::server::InMemoryTaskRunRepository task_run_repository;
    labbridge::server::InMemoryResultRepository result_repository;
    labbridge::server::InMemoryQcRepository qc_repository;
    labbridge::server::InMemoryAlertRepository alert_repository;
    labbridge::server::InMemoryAgentReportReceiptRepository receipt_repository;

    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{node_repository, config_repository};
    labbridge::server::TaskRunService task_run_service{config_repository, task_run_repository};
    labbridge::server::ResultService result_service{task_run_repository, result_repository};
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
    labbridge::server::ControlPlaneQueryService query_service{
        node_repository,
        config_repository,
        task_run_repository,
        result_repository,
        qc_repository,
        alert_repository};

    const std::string node_code = "lab-node-report-016";
    const std::string other_node_code = "lab-node-report-016-other";

    ASSERT_TRUE(node_service.register_node({node_code, "report-node", labbridge::core::kVersion}).ok);
    ASSERT_TRUE(node_service.register_node({other_node_code, "report-other-node", labbridge::core::kVersion}).ok);
    ASSERT_TRUE(node_service.accept_heartbeat({
               node_code,
               labbridge::core::kVersion,
               "2026-06-02 10:00:00+08",
           }).ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase16 local csv dir",
        "{}",
        true,
    });
    ASSERT_TRUE(data_source.status.ok);

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase16 agent reported csv",
        "collect_parse_qc",
        "manual",
        "csv_observation",
        "basic",
        true,
    });
    ASSERT_TRUE(task.status.ok);

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-06-02 10:01:00+08",
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
                "2026-06-02 09:59:00+08",
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
        "2026-06-02 10:03:00+08",
        1,
        0,
        1,
        "invalid finish status",
        {},
    });
    ASSERT_TRUE(!invalid_status_report.status.ok);

    const auto report = agent_report_service.accept_task_run_report({
        started.id,
        node_code,
        "phase16-report",
        labbridge::core::TaskRunStatus::Failed,
        "2026-06-02 10:03:00+08",
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
                    "2026-06-02 10:00:00+08",
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

    const auto detail = query_service.find_task_run_detail(node_code, started.id);
    ASSERT_TRUE(detail.status.ok);
    ASSERT_TRUE(detail.task_run.has_value());
    ASSERT_TRUE(detail.task_run->status == labbridge::core::TaskRunStatus::Failed);
    ASSERT_TRUE(detail.task_run->items_total == 1);
    ASSERT_TRUE(detail.task_run->items_failed == 1);
    ASSERT_TRUE(detail.raw_files.size() == 1);
    ASSERT_TRUE(detail.parsed_records.size() == 1);
    ASSERT_TRUE(contains_qc_result(detail.qc_results, report.qc_result_ids.front()));
    ASSERT_TRUE(contains_qc_result(detail.qc_results, report.qc_result_ids.back()));
    ASSERT_TRUE(contains_alert(detail.alerts, report.alert_ids.front()));

    const auto* raw_file = find_raw_file(detail.raw_files, manifest.raw_file_ids.front());
    ASSERT_TRUE(raw_file != nullptr);
    ASSERT_TRUE(raw_file->node_code == node_code);
    ASSERT_TRUE(raw_file->storage_path == "/archive/phase16/phase16_observation.csv");
    ASSERT_TRUE(raw_file->ingest_status == "archived");

    ASSERT_TRUE(contains_id(report.parsed_record_ids, detail.parsed_records.front().id));
    ASSERT_TRUE(detail.parsed_records.front().raw_file_id == manifest.raw_file_ids.front());

    const auto node_overview = query_service.find_node_overview(node_code);
    ASSERT_TRUE(node_overview.status.ok);
    ASSERT_TRUE(contains_alert(node_overview.alerts, report.alert_ids.front()));

}
