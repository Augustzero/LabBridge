#include "labbridge/core/version.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/agent_report_executor.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/postgres/storage_mapping.h"
#include "labbridge/server/application/task_run_service.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

int count_rows(labbridge::server::ISqlSession& session,
               const std::string& sql,
               const labbridge::server::SqlParams& params) {
    const auto row = session.query_one(sql, params);
    assert(row.has_value());
    return labbridge::server::storage::int_or_zero(*row, "count");
}

labbridge::server::RawFileManifestRequest make_manifest(
    const std::string& task_run_id,
    const std::string& node_code,
    const std::string& key,
    const std::string& suffix) {
    labbridge::server::RawFileManifestRequest request;
    request.task_run_id = task_run_id;
    request.node_code = node_code;
    request.idempotency_key = key;
    request.files.push_back({
        "phase18_" + suffix + ".csv",
        "phase18-hash-" + suffix,
        "/archive/phase18/" + suffix + ".csv",
        640,
        "2026-07-17 11:00:00+08",
        "archived",
    });
    return request;
}

labbridge::server::TaskRunReportRequest make_report(
    const std::string& task_run_id,
    const std::string& node_code,
    const std::string& key,
    const std::string& raw_file_id,
    const std::string& qc_rule_id) {
    labbridge::server::TaskRunReportRequest request;
    request.task_run_id = task_run_id;
    request.node_code = node_code;
    request.idempotency_key = key;
    request.status = labbridge::core::TaskRunStatus::Failed;
    request.finished_at = "2026-07-17 11:03:00+08";
    request.items_total = 1;
    request.items_failed = 1;
    request.error_summary = "qc failed";
    request.parsed_records.push_back({
        raw_file_id,
        {
            "station-real",
            "device-real",
            "2026-07-17 11:00:00+08",
            "{\"temperature\":48.5}",
        },
        "parsed",
        {
            {
                qc_rule_id,
                "failed",
                "failed",
                "temperature is outside configured range",
            },
        },
    });
    return request;
}

}  // namespace

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string{connection_info}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL "
                     "agent report reliability smoke test\n";
        return 77;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::PostgresResultRepository result_repository{session};
    labbridge::server::PostgresQcRepository qc_repository{session};

    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{
        node_repository,
        config_repository};
    labbridge::server::TaskRunService task_run_service{
        config_repository,
        task_run_repository};
    labbridge::server::ResultService result_service{
        task_run_repository,
        result_repository};
    labbridge::server::QcService qc_service{result_repository, qc_repository};

    const std::string node_code = "lab-node-real-report-018";
    assert(node_service.register_node(
               {node_code, "phase18-real-report-node", labbridge::core::kVersion})
               .ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase18 real local csv dir",
        "{}",
        true,
    });
    assert(data_source.status.ok);

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase18 reliable real report",
        "collect_parse_qc",
        "* * * * *",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);

    const auto rule = qc_service.create_rule({
        "phase18 real temperature range",
        "range_check",
        "{}",
        true,
    });
    assert(rule.status.ok);

    labbridge::server::PostgresAgentReportExecutor executor{connection_info};

    const auto rollback_run = task_run_service.start({
        node_code,
        task.id,
        "2026-07-17 11:01:00+08",
        "phase18_rollback",
    });
    assert(rollback_run.status.ok);

    const auto rollback_manifest_key =
        "phase18-manifest-rollback-" + rollback_run.id;
    const auto rollback_manifest = executor.accept_raw_file_manifest(
        make_manifest(
            rollback_run.id,
            node_code,
            rollback_manifest_key,
            "rollback"));
    assert(rollback_manifest.status.ok);
    assert(!rollback_manifest.replayed);
    assert(rollback_manifest.raw_file_ids.size() == 1);

    const auto manifest_replay = executor.accept_raw_file_manifest(
        make_manifest(
            rollback_run.id,
            node_code,
            rollback_manifest_key,
            "rollback"));
    assert(manifest_replay.status.ok);
    assert(manifest_replay.replayed);
    assert(manifest_replay.raw_file_ids == rollback_manifest.raw_file_ids);

    const auto rollback_report_key =
        "phase18-report-rollback-" + rollback_run.id;
    auto invalid_report = make_report(
        rollback_run.id,
        node_code,
        rollback_report_key,
        rollback_manifest.raw_file_ids.front(),
        rule.id);
    invalid_report.parsed_records.push_back({
        "0",
        {"station-invalid", "device-invalid", {}, "{}"},
        "parsed",
        {},
    });

    const auto invalid_result = executor.accept_task_run_report(invalid_report);
    assert(!invalid_result.status.ok);
    assert(count_rows(
               session,
               "SELECT count(*)::text AS count FROM parsed_records "
               "WHERE task_run_id = $1::bigint",
               {rollback_run.id}) == 0);
    assert(count_rows(
               session,
               "SELECT count(*)::text AS count FROM agent_report_receipts "
               "WHERE task_run_id = $1::bigint AND request_type = 'task_run_report'",
               {rollback_run.id}) == 0);
    const auto still_running = task_run_service.find_run(rollback_run.id);
    assert(still_running.has_value());
    assert(still_running->status == labbridge::core::TaskRunStatus::Running);

    const auto valid_report = make_report(
        rollback_run.id,
        node_code,
        rollback_report_key,
        rollback_manifest.raw_file_ids.front(),
        rule.id);
    const auto report = executor.accept_task_run_report(valid_report);
    assert(report.status.ok);
    assert(!report.replayed);
    assert(report.parsed_record_ids.size() == 1);
    assert(report.qc_result_ids.size() == 1);
    assert(report.alert_ids.size() == 1);

    const auto report_replay = executor.accept_task_run_report(valid_report);
    assert(report_replay.status.ok);
    assert(report_replay.replayed);
    assert(report_replay.parsed_record_ids == report.parsed_record_ids);
    assert(report_replay.qc_result_ids == report.qc_result_ids);
    assert(report_replay.alert_ids == report.alert_ids);
    assert(count_rows(
               session,
               "SELECT count(*)::text AS count FROM parsed_records "
               "WHERE task_run_id = $1::bigint",
               {rollback_run.id}) == 1);

    auto changed_report = valid_report;
    changed_report.error_summary = "different request";
    const auto changed_result = executor.accept_task_run_report(changed_report);
    assert(!changed_result.status.ok);
    assert(changed_result.status.code == labbridge::core::StatusCode::Conflict);

    auto second_key_report = valid_report;
    second_key_report.idempotency_key =
        "phase18-report-second-" + rollback_run.id;
    const auto second_key_result =
        executor.accept_task_run_report(second_key_report);
    assert(!second_key_result.status.ok);
    assert(second_key_result.status.code == labbridge::core::StatusCode::Conflict);

    const auto concurrent_manifest_run = task_run_service.start({
        node_code,
        task.id,
        "2026-07-17 11:10:00+08",
        "phase18_concurrent_manifest",
    });
    assert(concurrent_manifest_run.status.ok);
    const auto concurrent_manifest = make_manifest(
        concurrent_manifest_run.id,
        node_code,
        "phase18-manifest-concurrent-" + concurrent_manifest_run.id,
        "concurrent_manifest");

    labbridge::server::RawFileManifestResult manifest_result_a;
    labbridge::server::RawFileManifestResult manifest_result_b;
    std::thread manifest_thread_a([&] {
        manifest_result_a = executor.accept_raw_file_manifest(concurrent_manifest);
    });
    std::thread manifest_thread_b([&] {
        manifest_result_b = executor.accept_raw_file_manifest(concurrent_manifest);
    });
    manifest_thread_a.join();
    manifest_thread_b.join();
    assert(manifest_result_a.status.ok);
    assert(manifest_result_b.status.ok);
    assert(manifest_result_a.replayed != manifest_result_b.replayed);
    assert(manifest_result_a.raw_file_ids == manifest_result_b.raw_file_ids);
    assert(count_rows(
               session,
               "SELECT count(*)::text AS count FROM raw_files "
               "WHERE task_run_id = $1::bigint",
               {concurrent_manifest_run.id}) == 1);

    const auto concurrent_report_run = task_run_service.start({
        node_code,
        task.id,
        "2026-07-17 11:20:00+08",
        "phase18_concurrent_report",
    });
    assert(concurrent_report_run.status.ok);
    const auto report_manifest = executor.accept_raw_file_manifest(
        make_manifest(
            concurrent_report_run.id,
            node_code,
            "phase18-manifest-report-" + concurrent_report_run.id,
            "concurrent_report"));
    assert(report_manifest.status.ok);

    const auto concurrent_report_a = make_report(
        concurrent_report_run.id,
        node_code,
        "phase18-report-concurrent-a-" + concurrent_report_run.id,
        report_manifest.raw_file_ids.front(),
        rule.id);
    auto concurrent_report_b = concurrent_report_a;
    concurrent_report_b.idempotency_key =
        "phase18-report-concurrent-b-" + concurrent_report_run.id;

    labbridge::server::TaskRunReportResult report_result_a;
    labbridge::server::TaskRunReportResult report_result_b;
    std::thread report_thread_a([&] {
        report_result_a = executor.accept_task_run_report(concurrent_report_a);
    });
    std::thread report_thread_b([&] {
        report_result_b = executor.accept_task_run_report(concurrent_report_b);
    });
    report_thread_a.join();
    report_thread_b.join();
    assert(report_result_a.status.ok != report_result_b.status.ok);
    const auto& report_conflict =
        report_result_a.status.ok ? report_result_b : report_result_a;
    assert(report_conflict.status.code == labbridge::core::StatusCode::Conflict);
    assert(count_rows(
               session,
               "SELECT count(*)::text AS count FROM parsed_records "
               "WHERE task_run_id = $1::bigint",
               {concurrent_report_run.id}) == 1);
    assert(count_rows(
               session,
               "SELECT count(*)::text AS count FROM agent_report_receipts "
               "WHERE task_run_id = $1::bigint AND request_type = 'task_run_report'",
               {concurrent_report_run.id}) == 1);
    return 0;
}
