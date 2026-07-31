#include "labbridge/core/version.h"
#include "labbridge/server/agent_report_service.h"
#include "labbridge/server/alert_service.h"
#include "labbridge/server/config_service.h"
#include "labbridge/server/libpq_sql_session.h"
#include "labbridge/server/node_service.h"
#include "labbridge/server/postgres_alert_repository.h"
#include "labbridge/server/postgres_config_repository.h"
#include "labbridge/server/postgres_node_repository.h"
#include "labbridge/server/postgres_qc_repository.h"
#include "labbridge/server/postgres_result_repository.h"
#include "labbridge/server/postgres_task_run_repository.h"
#include "labbridge/server/qc_service.h"
#include "labbridge/server/query_service.h"
#include "labbridge/server/result_service.h"
#include "labbridge/server/storage_mapping.h"
#include "labbridge/server/task_run_service.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

}  // namespace

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL agent report smoke test\n";
        return 77;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::PostgresResultRepository result_repository{session};
    labbridge::server::PostgresQcRepository qc_repository{session};
    labbridge::server::PostgresAlertRepository alert_repository{session};
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

    const std::string node_code = "lab-node-real-report-016";
    const std::string other_node_code = "lab-node-real-report-016-other";

    assert(node_service.register_node({node_code, "real-report-node", labbridge::core::kVersion}).ok);
    assert(node_service.register_node({other_node_code, "real-report-other-node", labbridge::core::kVersion}).ok);
    assert(node_service.accept_heartbeat({
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
    assert(data_source.status.ok);
    assert(!data_source.id.empty());

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
    assert(task.status.ok);
    assert(!task.id.empty());

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-06-02 10:01:00+08",
        "agent_report",
    });
    assert(started.status.ok);
    assert(!started.id.empty());

    const auto wrong_node_manifest = agent_report_service.accept_raw_file_manifest({
        started.id,
        other_node_code,
        "phase16-real-wrong-node-manifest",
        {},
    });
    assert(!wrong_node_manifest.status.ok);

    const auto manifest = agent_report_service.accept_raw_file_manifest({
        started.id,
        node_code,
        "phase16-real-manifest",
        {
            {
                "phase16_observation.csv",
                "phase16-real-hash-archived",
                "/archive/phase16/phase16_observation.csv",
                256,
                "2026-06-02 09:59:00+08",
                "archived",
            },
        },
    });
    assert(manifest.status.ok);
    assert(manifest.raw_file_ids.size() == 1);

    const auto rule = qc_service.create_rule({
        "phase16 reported temperature range",
        "range_check",
        "{}",
        true,
    });
    assert(rule.status.ok);
    assert(!rule.id.empty());

    const auto report = agent_report_service.accept_task_run_report({
        started.id,
        node_code,
        "phase16-real-report",
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
    assert(report.status.ok);
    assert(report.parsed_record_ids.size() == 1);
    assert(report.qc_result_ids.size() == 2);
    assert(report.alert_ids.size() == 1);

    const auto detail = query_service.find_task_run_detail(node_code, started.id);
    assert(detail.status.ok);
    assert(detail.task_run.has_value());
    assert(detail.task_run->status == labbridge::core::TaskRunStatus::Failed);
    assert(detail.raw_files.size() == 1);
    assert(detail.parsed_records.size() == 1);
    assert(contains_qc_result(detail.qc_results, report.qc_result_ids.front()));
    assert(contains_qc_result(detail.qc_results, report.qc_result_ids.back()));
    assert(contains_alert(detail.alerts, report.alert_ids.front()));

    const auto persisted = session.query_one(
        "SELECT n.node_code, tr.id::text AS task_run_id, tr.status, "
        "rf.id::text AS raw_file_id, rf.storage_path, rf.ingest_status, "
        "pr.id::text AS parsed_record_id, pr.raw_file_id::text AS parsed_raw_file_id, "
        "(SELECT count(*)::text FROM qc_results qr WHERE qr.parsed_record_id = pr.id) AS qc_count, "
        "(SELECT count(*)::text FROM alerts al WHERE al.task_run_id = tr.id) AS alert_count "
        "FROM raw_files rf "
        "JOIN task_runs tr ON tr.id = rf.task_run_id "
        "JOIN nodes n ON n.id = rf.node_id "
        "JOIN parsed_records pr ON pr.raw_file_id = rf.id "
        "WHERE n.node_code = $1 AND tr.id = $2::bigint AND rf.id = $3::bigint "
        "LIMIT 1",
        {node_code, started.id, manifest.raw_file_ids.front()});

    assert(persisted.has_value());
    assert(labbridge::server::storage::value_or_empty(*persisted, "node_code") == node_code);
    assert(labbridge::server::storage::value_or_empty(*persisted, "task_run_id") == started.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "status") == "failed");
    assert(labbridge::server::storage::value_or_empty(*persisted, "raw_file_id") == manifest.raw_file_ids.front());
    assert(labbridge::server::storage::value_or_empty(*persisted, "storage_path") == "/archive/phase16/phase16_observation.csv");
    assert(labbridge::server::storage::value_or_empty(*persisted, "ingest_status") == "archived");
    assert(labbridge::server::storage::value_or_empty(*persisted, "parsed_record_id") == report.parsed_record_ids.front());
    assert(labbridge::server::storage::value_or_empty(*persisted, "parsed_raw_file_id") == manifest.raw_file_ids.front());
    assert(labbridge::server::storage::value_or_empty(*persisted, "qc_count") == "2");
    assert(labbridge::server::storage::value_or_empty(*persisted, "alert_count") == "1");

    return 0;
}
