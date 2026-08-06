#include "support/server/in_memory_repositories.h"
#include "labbridge/core/version.h"
#include "labbridge/server/http/agent_report_http_controller.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/query_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/application/task_run_service.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/writer.h>

#include <cassert>
#include <stdexcept>
#include <string>

namespace {

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

drogon::HttpResponsePtr invoke_manifest(
    const labbridge::server::AgentReportHttpController& controller,
    const std::string& body,
    bool json_content_type = true) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    if (json_content_type) {
        request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    request->setBody(body);

    drogon::HttpResponsePtr response;
    controller.post_raw_file_manifest(
        request,
        [&response](const drogon::HttpResponsePtr& current) {
            response = current;
        });
    assert(response != nullptr);
    return response;
}

drogon::HttpResponsePtr invoke_report(
    const labbridge::server::AgentReportHttpController& controller,
    const std::string& body) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(body);

    drogon::HttpResponsePtr response;
    controller.post_task_run_report(
        request,
        [&response](const drogon::HttpResponsePtr& current) {
            response = current;
        });
    assert(response != nullptr);
    return response;
}

const Json::Value& response_json(const drogon::HttpResponsePtr& response) {
    const auto& json = response->getJsonObject();
    assert(json != nullptr);
    return *json;
}

void assert_error(const drogon::HttpResponsePtr& response,
                  drogon::HttpStatusCode expected_status,
                  const std::string& expected_code) {
    assert(response->statusCode() == expected_status);
    const auto& json = response_json(response);
    assert(!json["ok"].asBool());
    assert(json["error"]["code"].asString() == expected_code);
    assert(!json["error"]["message"].asString().empty());
}

Json::Value manifest_body(const std::string& task_run_id,
                          const std::string& node_code) {
    Json::Value body;
    body["task_run_id"] = task_run_id;
    body["node_code"] = node_code;
    body["idempotency_key"] = "phase17-manifest";

    Json::Value file;
    file["original_name"] = "phase17_observation.csv";
    file["file_hash"] = "phase17-local-hash";
    file["storage_path"] = "/archive/phase17/phase17_observation.csv";
    file["size_bytes"] = Json::Int64{320};
    file["source_mtime"] = "2026-07-16 10:00:00+08";
    file["ingest_status"] = "archived";
    body["files"].append(std::move(file));
    return body;
}

Json::Value report_body(const std::string& task_run_id,
                        const std::string& node_code,
                        const std::string& raw_file_id,
                        const std::string& qc_rule_id) {
    Json::Value body;
    body["task_run_id"] = task_run_id;
    body["node_code"] = node_code;
    body["idempotency_key"] = "phase17-report";
    body["status"] = "failed";
    body["finished_at"] = "2026-07-16 10:03:00+08";
    body["items_total"] = 1;
    body["items_success"] = 0;
    body["items_failed"] = 1;
    body["error_summary"] = "qc failed";

    Json::Value parsed;
    parsed["raw_file_id"] = raw_file_id;
    parsed["station_code"] = "station-a";
    parsed["device_code"] = "device-a";
    parsed["record_time"] = "2026-07-16 10:00:00+08";
    parsed["payload_json"] = "[48.5]";
    parsed["parse_status"] = "parsed";

    Json::Value passed;
    passed["qc_rule_id"] = qc_rule_id;
    passed["level"] = "pass";
    passed["result"] = "passed";
    passed["message"] = "humidity is in range";
    parsed["qc_results"].append(std::move(passed));

    Json::Value failed;
    failed["qc_rule_id"] = qc_rule_id;
    failed["level"] = "failed";
    failed["result"] = "failed";
    failed["message"] = "temperature is outside configured range";
    parsed["qc_results"].append(std::move(failed));

    body["parsed_records"].append(std::move(parsed));
    return body;
}

}  // namespace

int main() {
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
    labbridge::server::ControlPlaneQueryService query_service{
        node_repository,
        config_repository,
        task_run_repository,
        result_repository,
        qc_repository,
        alert_repository};

    labbridge::server::AgentReportHttpController controller{
        [&agent_report_service](
            const labbridge::server::RawFileManifestRequest& request) {
            return agent_report_service.accept_raw_file_manifest(request);
        },
        [&agent_report_service](
            const labbridge::server::TaskRunReportRequest& request) {
            return agent_report_service.accept_task_run_report(request);
        }};

    assert_error(
        invoke_manifest(controller, "{}", false),
        drogon::k415UnsupportedMediaType,
        "unsupported_media_type");
    assert_error(
        invoke_manifest(controller, "{"),
        drogon::k400BadRequest,
        "invalid_argument");

    auto missing_idempotency_key = manifest_body("missing-run", "phase17-node");
    missing_idempotency_key.removeMember("idempotency_key");
    assert_error(
        invoke_manifest(controller, write_json(missing_idempotency_key)),
        drogon::k400BadRequest,
        "invalid_argument");

    Json::Value wrong_type;
    wrong_type["task_run_id"] = 17;
    wrong_type["node_code"] = "phase17-node";
    assert_error(
        invoke_manifest(controller, write_json(wrong_type)),
        drogon::k400BadRequest,
        "invalid_argument");

    const std::string node_code = "lab-node-http-report-017";
    const std::string other_node_code = "lab-node-http-report-017-other";
    assert(node_service.register_node(
               {node_code, "phase17-http-node", labbridge::core::kVersion})
               .ok);
    assert(node_service.register_node(
               {other_node_code, "phase17-http-other-node", labbridge::core::kVersion})
               .ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase17 local csv dir",
        "{}",
        true,
    });
    assert(data_source.status.ok);

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase17 HTTP reported csv",
        "collect_parse_qc",
        "manual",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-07-16 10:01:00+08",
        "http_report",
    });
    assert(started.status.ok);

    assert_error(
        invoke_manifest(controller, write_json(manifest_body("missing-run", node_code))),
        drogon::k404NotFound,
        "not_found");
    assert_error(
        invoke_manifest(
            controller,
            write_json(manifest_body(started.id, other_node_code))),
        drogon::k409Conflict,
        "conflict");

    const auto manifest_response =
        invoke_manifest(controller, write_json(manifest_body(started.id, node_code)));
    assert(manifest_response->statusCode() == drogon::k201Created);
    const auto& manifest_json = response_json(manifest_response);
    assert(manifest_json["ok"].asBool());
    assert(!manifest_json["data"]["replayed"].asBool());
    assert(manifest_json["data"]["raw_file_ids"].size() == 1);
    const auto raw_file_id =
        manifest_json["data"]["raw_file_ids"][Json::ArrayIndex{0}].asString();
    assert(!raw_file_id.empty());

    const auto manifest_replay =
        invoke_manifest(controller, write_json(manifest_body(started.id, node_code)));
    assert(manifest_replay->statusCode() == drogon::k201Created);
    assert(response_json(manifest_replay)["data"]["replayed"].asBool());
    assert(response_json(manifest_replay)["data"]["raw_file_ids"][Json::ArrayIndex{0}]
               .asString() == raw_file_id);

    const auto rule = qc_service.create_rule({
        "phase17 reported temperature range",
        "range_check",
        "{}",
        true,
    });
    assert(rule.status.ok);

    auto invalid_status =
        report_body(started.id, node_code, raw_file_id, rule.id);
    invalid_status["status"] = "running";
    assert_error(
        invoke_report(controller, write_json(invalid_status)),
        drogon::k400BadRequest,
        "invalid_argument");

    auto invalid_payload =
        report_body(started.id, node_code, raw_file_id, rule.id);
    invalid_payload["parsed_records"][Json::ArrayIndex{0}]["payload_json"] = "{";
    assert_error(
        invoke_report(controller, write_json(invalid_payload)),
        drogon::k400BadRequest,
        "invalid_argument");

    const auto report_response = invoke_report(
        controller,
        write_json(report_body(started.id, node_code, raw_file_id, rule.id)));
    assert(report_response->statusCode() == drogon::k200OK);
    const auto& report_json = response_json(report_response);
    assert(report_json["ok"].asBool());
    assert(!report_json["data"]["replayed"].asBool());
    assert(report_json["data"]["parsed_record_ids"].size() == 1);
    assert(report_json["data"]["qc_result_ids"].size() == 2);
    assert(report_json["data"]["alert_ids"].size() == 1);

    const auto report_replay = invoke_report(
        controller,
        write_json(report_body(started.id, node_code, raw_file_id, rule.id)));
    assert(report_replay->statusCode() == drogon::k200OK);
    const auto& report_replay_json = response_json(report_replay);
    assert(report_replay_json["data"]["replayed"].asBool());
    assert(report_replay_json["data"]["parsed_record_ids"] ==
           report_json["data"]["parsed_record_ids"]);

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

    labbridge::server::AgentReportHttpController throwing_controller{
        [](const labbridge::server::RawFileManifestRequest&)
            -> labbridge::server::RawFileManifestResult {
            throw std::runtime_error("database password must stay private");
        },
        [](const labbridge::server::TaskRunReportRequest&)
            -> labbridge::server::TaskRunReportResult {
            throw std::runtime_error("unexpected report failure");
        }};
    const auto internal_response = invoke_manifest(
        throwing_controller,
        write_json(manifest_body(started.id, node_code)));
    assert_error(
        internal_response,
        drogon::k500InternalServerError,
        "internal_error");
    assert(response_json(internal_response)["error"]["message"].asString() ==
           "internal server error");
    assert(response_json(internal_response)["error"]["message"]
               .asString()
               .find("password") == std::string::npos);

    return 0;
}
