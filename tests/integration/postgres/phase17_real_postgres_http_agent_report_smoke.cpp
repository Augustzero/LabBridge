#include "labbridge/core/version.h"
#include "labbridge/server/http/agent_report_http_controller.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/agent_report_executor.h"
#include "labbridge/server/postgres/alert_repository.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/query_service.h"
#include "labbridge/server/postgres/storage_mapping.h"
#include "labbridge/server/application/task_run_service.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/writer.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

drogon::HttpResponsePtr invoke(
    const labbridge::server::AgentReportHttpController& controller,
    const Json::Value& body,
    bool manifest) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(write_json(body));

    drogon::HttpResponsePtr response;
    auto callback = [&response](const drogon::HttpResponsePtr& current) {
        response = current;
    };
    if (manifest) {
        controller.post_raw_file_manifest(request, std::move(callback));
    } else {
        controller.post_task_run_report(request, std::move(callback));
    }
    assert(response != nullptr);
    return response;
}

const Json::Value& response_json(const drogon::HttpResponsePtr& response) {
    const auto& json = response->getJsonObject();
    assert(json != nullptr);
    return *json;
}

}  // namespace

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL HTTP agent report smoke test\n";
        return 77;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::PostgresResultRepository result_repository{session};
    labbridge::server::PostgresQcRepository qc_repository{session};
    labbridge::server::PostgresAlertRepository alert_repository{session};

    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{
        node_repository,
        config_repository};
    labbridge::server::TaskRunService task_run_service{
        config_repository,
        task_run_repository};
    labbridge::server::QcService qc_service{result_repository, qc_repository};
    labbridge::server::ControlPlaneQueryService query_service{
        node_repository,
        config_repository,
        task_run_repository,
        result_repository,
        qc_repository,
        alert_repository};

    const std::string node_code = "lab-node-real-http-report-017";
    assert(node_service.register_node(
               {node_code, "phase17-real-http-node", labbridge::core::kVersion})
               .ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase17 real HTTP local csv dir",
        "{}",
        true,
    });
    assert(data_source.status.ok);

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase17 real HTTP reported csv",
        "collect_parse_qc",
        "* * * * *",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-07-16 11:01:00+08",
        "http_report",
    });
    assert(started.status.ok);

    const auto rule = qc_service.create_rule({
        "phase17 real HTTP temperature range",
        "range_check",
        "{}",
        true,
    });
    assert(rule.status.ok);

    auto executor =
        std::make_shared<labbridge::server::PostgresAgentReportExecutor>(
            connection_info);
    labbridge::server::AgentReportHttpController controller{
        [executor](const labbridge::server::RawFileManifestRequest& request) {
            return executor->accept_raw_file_manifest(request);
        },
        [executor](const labbridge::server::TaskRunReportRequest& request) {
            return executor->accept_task_run_report(request);
        }};

    Json::Value manifest;
    manifest["task_run_id"] = started.id;
    manifest["node_code"] = node_code;
    manifest["idempotency_key"] =
        "phase17-real-http-manifest-" + started.id;
    Json::Value file;
    file["original_name"] = "phase17_real_http_observation.csv";
    file["file_hash"] = "phase17-real-http-hash";
    file["storage_path"] =
        "/archive/phase17/phase17_real_http_observation.csv";
    file["size_bytes"] = Json::Int64{384};
    file["source_mtime"] = "2026-07-16 11:00:00+08";
    file["ingest_status"] = "archived";
    manifest["files"].append(std::move(file));

    const auto manifest_response = invoke(controller, manifest, true);
    assert(manifest_response->statusCode() == drogon::k201Created);
    const auto& manifest_json = response_json(manifest_response);
    assert(manifest_json["ok"].asBool());
    const auto raw_file_id =
        manifest_json["data"]["raw_file_ids"][Json::ArrayIndex{0}].asString();
    assert(!raw_file_id.empty());

    Json::Value report;
    report["task_run_id"] = started.id;
    report["node_code"] = node_code;
    report["idempotency_key"] =
        "phase17-real-http-report-" + started.id;
    report["status"] = "failed";
    report["finished_at"] = "2026-07-16 11:03:00+08";
    report["items_total"] = 1;
    report["items_success"] = 0;
    report["items_failed"] = 1;
    report["error_summary"] = "qc failed";

    Json::Value parsed;
    parsed["raw_file_id"] = raw_file_id;
    parsed["station_code"] = "station-real";
    parsed["device_code"] = "device-real";
    parsed["record_time"] = "2026-07-16 11:00:00+08";
    parsed["payload_json"] = "{\"temperature\":48.5}";
    parsed["parse_status"] = "parsed";

    Json::Value passed;
    passed["qc_rule_id"] = rule.id;
    passed["level"] = "pass";
    passed["result"] = "passed";
    passed["message"] = "humidity is in range";
    parsed["qc_results"].append(std::move(passed));

    Json::Value failed;
    failed["qc_rule_id"] = rule.id;
    failed["level"] = "failed";
    failed["result"] = "failed";
    failed["message"] = "temperature is outside configured range";
    parsed["qc_results"].append(std::move(failed));

    report["parsed_records"].append(std::move(parsed));

    const auto report_response = invoke(controller, report, false);
    assert(report_response->statusCode() == drogon::k200OK);
    const auto& report_json = response_json(report_response);
    assert(report_json["ok"].asBool());
    assert(report_json["data"]["parsed_record_ids"].size() == 1);
    assert(report_json["data"]["qc_result_ids"].size() == 2);
    assert(report_json["data"]["alert_ids"].size() == 1);

    const auto detail = query_service.find_task_run_detail(node_code, started.id);
    assert(detail.status.ok);
    assert(detail.task_run.has_value());
    assert(detail.task_run->status == labbridge::core::TaskRunStatus::Failed);
    assert(detail.raw_files.size() == 1);
    assert(detail.parsed_records.size() == 1);
    assert(detail.qc_results.size() == 2);
    assert(detail.alerts.size() == 1);
    assert(detail.raw_files.front().id == raw_file_id);
    assert(detail.parsed_records.front().raw_file_id == raw_file_id);

    const auto persisted = session.query_one(
        "SELECT n.node_code, tr.id::text AS task_run_id, tr.status, "
        "rf.id::text AS raw_file_id, rf.storage_path, rf.ingest_status, "
        "pr.id::text AS parsed_record_id, "
        "(SELECT count(*)::text FROM qc_results qr "
        " WHERE qr.parsed_record_id = pr.id) AS qc_count, "
        "(SELECT count(*)::text FROM alerts al "
        " WHERE al.task_run_id = tr.id) AS alert_count "
        "FROM raw_files rf "
        "JOIN task_runs tr ON tr.id = rf.task_run_id "
        "JOIN nodes n ON n.id = rf.node_id "
        "JOIN parsed_records pr ON pr.raw_file_id = rf.id "
        "WHERE n.node_code = $1 AND tr.id = $2::bigint "
        "AND rf.id = $3::bigint LIMIT 1",
        {node_code, started.id, raw_file_id});

    assert(persisted.has_value());
    assert(labbridge::server::storage::value_or_empty(
               *persisted, "node_code") == node_code);
    assert(labbridge::server::storage::value_or_empty(
               *persisted, "task_run_id") == started.id);
    assert(labbridge::server::storage::value_or_empty(
               *persisted, "status") == "failed");
    assert(labbridge::server::storage::value_or_empty(
               *persisted, "storage_path") ==
           "/archive/phase17/phase17_real_http_observation.csv");
    assert(labbridge::server::storage::value_or_empty(
               *persisted, "ingest_status") == "archived");
    assert(labbridge::server::storage::value_or_empty(
               *persisted, "qc_count") == "2");
    assert(labbridge::server::storage::value_or_empty(
               *persisted, "alert_count") == "1");

    return 0;
}
