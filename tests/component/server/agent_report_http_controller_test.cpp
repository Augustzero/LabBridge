#include "support/server/in_memory_repositories.h"
#include "support/server/test_config_seed.h"
#include "labbridge/core/version.h"
#include "labbridge/server/http/agent_report_http_controller.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/application/task_run_service.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/writer.h>

#include <gtest/gtest.h>
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
    if (response == nullptr) {
        ADD_FAILURE() << "controller did not invoke response callback";
        return drogon::HttpResponse::newHttpResponse();
    }
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
    if (response == nullptr) {
        ADD_FAILURE() << "controller did not invoke response callback";
        return drogon::HttpResponse::newHttpResponse();
    }
    return response;
}

Json::Value response_json(const drogon::HttpResponsePtr& response) {
    if (response == nullptr) {
        ADD_FAILURE() << "response is null";
        return {};
    }
    const auto& json = response->getJsonObject();
    if (json == nullptr) {
        ADD_FAILURE() << "response body is not JSON";
        return {};
    }
    return *json;
}

void assert_error(const drogon::HttpResponsePtr& response,
                  drogon::HttpStatusCode expected_status,
                  const std::string& expected_code) {
    ASSERT_EQ(response->statusCode(), expected_status);
    const auto& json = response_json(response);
    EXPECT_FALSE(json["ok"].asBool());
    EXPECT_EQ(json["error"]["code"].asString(), expected_code);
    EXPECT_FALSE(json["error"]["message"].asString().empty());
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

TEST(AgentReportHttpControllerTest, MapsManifestReportReplayAndErrors) {
    labbridge::server::InMemoryNodeRepository node_repository;
    labbridge::server::InMemoryConfigRepository config_repository;
    labbridge::server::InMemoryTaskRunRepository task_run_repository;
    labbridge::server::InMemoryResultRepository result_repository;
    labbridge::server::InMemoryQcRepository qc_repository;
    labbridge::server::InMemoryAlertRepository alert_repository;
    labbridge::server::InMemoryAgentReportReceiptRepository receipt_repository;

    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::TaskRunService task_run_service{
        config_repository,
        task_run_repository};
    labbridge::server::ResultService result_service{
        task_run_repository,
        result_repository};
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
    EXPECT_TRUE(node_service.register_node(
               {node_code, "phase17-http-node", labbridge::core::kVersion})
               .ok);
    EXPECT_TRUE(node_service.register_node(
               {other_node_code, "phase17-http-other-node", labbridge::core::kVersion})
               .ok);

    const auto data_source =
        labbridge::server::test_support::create_local_csv_data_source(
            config_repository, node_code, "phase17 local csv dir");
    EXPECT_FALSE(data_source.empty());

    const auto task = labbridge::server::test_support::create_csv_task(
        config_repository, node_code, data_source,
        "phase17 HTTP reported csv");
    EXPECT_FALSE(task.empty());

    const auto started = task_run_service.start({
        node_code,
        task,
        "2026-07-16T02:01:00Z",
        "http_report",
    });
    EXPECT_TRUE(started.status.ok);

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
    EXPECT_TRUE(manifest_response->statusCode() == drogon::k201Created);
    const auto& manifest_json = response_json(manifest_response);
    EXPECT_TRUE(manifest_json["ok"].asBool());
    EXPECT_TRUE(!manifest_json["data"]["replayed"].asBool());
    EXPECT_TRUE(manifest_json["data"]["raw_file_ids"].size() == 1);
    const auto raw_file_id =
        manifest_json["data"]["raw_file_ids"][Json::ArrayIndex{0}].asString();
    EXPECT_TRUE(!raw_file_id.empty());

    const auto manifest_replay =
        invoke_manifest(controller, write_json(manifest_body(started.id, node_code)));
    EXPECT_TRUE(manifest_replay->statusCode() == drogon::k201Created);
    EXPECT_TRUE(response_json(manifest_replay)["data"]["replayed"].asBool());
    EXPECT_TRUE(response_json(manifest_replay)["data"]["raw_file_ids"][Json::ArrayIndex{0}]
               .asString() == raw_file_id);

    const auto rule = qc_service.create_rule({
        "phase17 reported temperature range",
        "range_check",
        "{}",
        true,
    });
    EXPECT_TRUE(rule.status.ok);

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
    EXPECT_TRUE(report_response->statusCode() == drogon::k200OK);
    const auto& report_json = response_json(report_response);
    EXPECT_TRUE(report_json["ok"].asBool());
    EXPECT_TRUE(!report_json["data"]["replayed"].asBool());
    EXPECT_TRUE(report_json["data"]["parsed_record_ids"].size() == 1);
    EXPECT_TRUE(report_json["data"]["qc_result_ids"].size() == 2);
    EXPECT_TRUE(report_json["data"]["alert_ids"].size() == 1);

    const auto report_replay = invoke_report(
        controller,
        write_json(report_body(started.id, node_code, raw_file_id, rule.id)));
    EXPECT_TRUE(report_replay->statusCode() == drogon::k200OK);
    const auto& report_replay_json = response_json(report_replay);
    EXPECT_TRUE(report_replay_json["data"]["replayed"].asBool());
    EXPECT_TRUE(report_replay_json["data"]["parsed_record_ids"] ==
           report_json["data"]["parsed_record_ids"]);

    // repository 逐对象回验，替代已删除的查询聚合服务。
    const auto finished = task_run_service.find_run(started.id);
    ASSERT_TRUE(finished.has_value());
    EXPECT_TRUE(finished->status == labbridge::core::TaskRunStatus::Failed);
    const auto stored_raw_file = result_repository.find_raw_file(raw_file_id);
    ASSERT_TRUE(stored_raw_file.has_value());
    EXPECT_TRUE(stored_raw_file->id == raw_file_id);
    const auto parsed_record_id =
        report_json["data"]["parsed_record_ids"][Json::ArrayIndex{0}].asString();
    const auto stored_record =
        result_repository.find_parsed_record(parsed_record_id);
    ASSERT_TRUE(stored_record.has_value());
    EXPECT_TRUE(stored_record->raw_file_id == raw_file_id);
    for (const auto& qc_result_id :
         report_json["data"]["qc_result_ids"]) {
        EXPECT_TRUE(
            qc_repository.find_result(qc_result_id.asString()).has_value());
    }
    EXPECT_TRUE(alert_repository.find_by_task_run(started.id).size() == 1);

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
    EXPECT_TRUE(response_json(internal_response)["error"]["message"].asString() ==
           "internal server error");
    EXPECT_TRUE(response_json(internal_response)["error"]["message"]
               .asString()
               .find("password") == std::string::npos);

}
