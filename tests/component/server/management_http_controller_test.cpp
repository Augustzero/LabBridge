#include "labbridge/server/http/management_http_controller.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <json/writer.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace labbridge::server;

template <typename T>
ManagementPageResult<T> empty_page() {
    return {labbridge::core::Status::success(), {}};
}

ManagementQueryHandlers empty_handlers() {
    ManagementQueryHandlers handlers;
    handlers.list_nodes = [](const NodeListRequest&) {
        return empty_page<ManagementNode>();
    };
    handlers.find_node = [](const std::string&) {
        return ManagementItemResult<ManagementNodeSummary>{
            labbridge::core::Status::failure(
                labbridge::core::StatusCode::NotFound, "node is not found"),
            std::nullopt};
    };
    handlers.list_data_sources = [](const NodeScopedListRequest&) {
        return empty_page<DataSourceRecord>();
    };
    handlers.list_qc_rules = [](const QcRuleListRequest&) {
        return empty_page<QcRuleRecord>();
    };
    handlers.list_tasks = [](const NodeScopedListRequest&) {
        return empty_page<TaskRecord>();
    };
    handlers.list_task_runs = [](const TaskRunListRequest&) {
        return empty_page<ManagementTaskRun>();
    };
    handlers.find_task_run = [](const std::string&, const std::string&) {
        return ManagementItemResult<ManagementTaskRunSummary>{
            labbridge::core::Status::failure(
                labbridge::core::StatusCode::NotFound,
                "task run is not found"),
            std::nullopt};
    };
    handlers.list_raw_files = [](const RunScopedListRequest&) {
        return empty_page<RawFileRecord>();
    };
    handlers.list_parsed_records = [](const RunScopedListRequest&) {
        return empty_page<ParsedRecordRecord>();
    };
    handlers.list_qc_results = [](const QcResultListRequest&) {
        return empty_page<QcResultRecord>();
    };
    handlers.list_alerts = [](const AlertListRequest&) {
        return empty_page<AlertRecord>();
    };
    return handlers;
}

ManagementCommandResult unused_command_result() {
    return {
        labbridge::core::Status::failure(
            labbridge::core::StatusCode::InvalidArgument,
            "command handler was not configured for this test"),
        {},
        {}};
}

ManagementCommandHandlers empty_command_handlers() {
    ManagementCommandHandlers handlers;
    handlers.create_data_source = [](const ManagementDataSourceCreateRequest&) {
        return unused_command_result();
    };
    handlers.create_qc_rule = [](const ManagementQcRuleCreateRequest&) {
        return unused_command_result();
    };
    handlers.create_task = [](const ManagementTaskCreateRequest&) {
        return unused_command_result();
    };
    handlers.set_task_enabled = [](const std::string&, bool) {
        return unused_command_result();
    };
    return handlers;
}

drogon::HttpRequestPtr request(
    std::initializer_list<std::pair<std::string, std::string>> parameters = {}) {
    auto value = drogon::HttpRequest::newHttpRequest();
    value->setMethod(drogon::Get);
    for (const auto& parameter : parameters) {
        value->setParameter(parameter.first, parameter.second);
    }
    return value;
}

drogon::HttpRequestPtr json_request(drogon::HttpMethod method,
                                    const Json::Value& body,
                                    bool application_json = true) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    auto value = drogon::HttpRequest::newHttpRequest();
    value->setMethod(method);
    if (application_json) {
        value->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    value->setBody(Json::writeString(builder, body));
    return value;
}

template <typename Invoke>
drogon::HttpResponsePtr invoke(Invoke operation) {
    drogon::HttpResponsePtr response;
    operation([&response](const drogon::HttpResponsePtr& current) {
        response = current;
    });
    EXPECT_NE(response, nullptr);
    return response;
}

const Json::Value& body(const drogon::HttpResponsePtr& response) {
    const auto& json = response->getJsonObject();
    EXPECT_NE(json, nullptr);
    return *json;
}

void expect_error(const drogon::HttpResponsePtr& response,
                  drogon::HttpStatusCode status,
                  const std::string& code) {
    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), status);
    EXPECT_FALSE(body(response)["ok"].asBool());
    EXPECT_EQ(body(response)["error"]["code"].asString(), code);
}

TEST(ManagementHttpControllerTest, ParsesWhitelistedPaginationAndFilters) {
    auto handlers = empty_handlers();
    NodeListRequest captured;
    handlers.list_nodes = [&captured](const NodeListRequest& current) {
        captured = current;
        NodeRecord record;
        record.id = "42";
        record.info.node_code = "node-http-025";
        record.info.name = "HTTP management node";
        record.info.agent_version = "0.1.0";
        record.status = labbridge::core::NodeStatus::Online;
        return ManagementPageResult<ManagementNode>{
            labbridge::core::Status::success(),
            {{{record, labbridge::core::NodeStatus::Offline}}, "42", true}};
    };
    ManagementHttpController controller{std::move(handlers), empty_command_handlers()};

    const auto response = invoke([&](auto callback) {
        controller.get_nodes(
            request({{"status", "offline"}, {"limit", "1"}, {"cursor", "99"}}),
            std::move(callback));
    });

    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    EXPECT_EQ(captured.status, "offline");
    EXPECT_EQ(captured.page.limit, 1);
    EXPECT_EQ(captured.page.cursor, "99");
    const auto& data = body(response)["data"];
    ASSERT_EQ(data["items"].size(), 1U);
    EXPECT_EQ(data["items"][0]["id"].asString(), "42");
    EXPECT_EQ(data["items"][0]["stored_status"].asString(), "online");
    EXPECT_EQ(data["items"][0]["effective_status"].asString(), "offline");
    EXPECT_TRUE(data["items"][0]["last_heartbeat_at"].isNull());
    EXPECT_TRUE(data["has_more"].asBool());
    EXPECT_EQ(data["next_cursor"].asString(), "42");

    expect_error(invoke([&](auto callback) {
        controller.get_nodes(request({{"offset", "20"}}), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    expect_error(invoke([&](auto callback) {
        controller.get_nodes(request({{"limit", "1.5"}}), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    expect_error(invoke([&](auto callback) {
        controller.get_tasks(request({{"node_code", "node-http-025"},
                                      {"enabled", "yes"}}),
                             std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    expect_error(invoke([&](auto callback) {
        controller.get_data_sources(request(), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
}

TEST(ManagementHttpControllerTest, MapsConfigurationAndSummaryObjects) {
    auto handlers = empty_handlers();
    handlers.find_node = [](const std::string& node_code) {
        NodeSummaryRecord summary;
        summary.node.id = "10";
        summary.node.info.node_code = node_code;
        summary.node.info.name = "node detail";
        summary.enabled_task_count = 2;
        summary.disabled_task_count = 1;
        summary.open_alert_count = 3;
        TaskRunRecord latest;
        latest.id = "90";
        latest.task_id = "20";
        latest.node_code = node_code;
        latest.status = labbridge::core::TaskRunStatus::Succeeded;
        summary.latest_task_run = latest;
        return ManagementItemResult<ManagementNodeSummary>{
            labbridge::core::Status::success(),
            ManagementNodeSummary{summary, labbridge::core::NodeStatus::Offline}};
    };
    handlers.list_data_sources = [](const NodeScopedListRequest& current) {
        DataSourceRecord source;
        source.id = "21";
        source.node_code = current.node_code;
        source.name = "instrument inbox";
        source.config_json = R"({"root_path":"/srv/inbox","extension":".csv"})";
        source.enabled = false;
        return ManagementPageResult<DataSourceRecord>{
            labbridge::core::Status::success(), {{{source}}, std::nullopt, false}};
    };
    handlers.list_qc_rules = [](const QcRuleListRequest&) {
        QcRuleRecord rule;
        rule.id = "31";
        rule.name = "required fields";
        rule.rule_type = "required_fields";
        rule.rule_config_json = "{}";
        return ManagementPageResult<QcRuleRecord>{
            labbridge::core::Status::success(), {{{rule}}, std::nullopt, false}};
    };
    handlers.list_tasks = [](const NodeScopedListRequest& current) {
        TaskRecord task;
        task.id = "41";
        task.node_code = current.node_code;
        task.data_source_id = "21";
        task.name = "CSV import";
        task.task_type = "local_file_import";
        task.schedule_expr = "*/5 * * * *";
        task.parser_type = "csv_observation";
        task.qc_profile = "default";
        task.qc_rule_ids = {"31", "32"};
        return ManagementPageResult<TaskRecord>{
            labbridge::core::Status::success(), {{{task}}, std::nullopt, false}};
    };
    ManagementHttpController controller{std::move(handlers), empty_command_handlers()};

    const auto node = invoke([&](auto callback) {
        controller.get_node(request(), "node-http-025", std::move(callback));
    });
    EXPECT_EQ(body(node)["data"]["enabled_task_count"].asInt(), 2);
    EXPECT_EQ(body(node)["data"]["latest_task_run"]["id"].asString(), "90");

    const auto sources = invoke([&](auto callback) {
        controller.get_data_sources(request({{"node_code", "node-http-025"},
                                             {"enabled", "false"}}),
                                    std::move(callback));
    });
    const auto& source = body(sources)["data"]["items"][0];
    EXPECT_TRUE(source["config"].isObject());
    EXPECT_EQ(source["config"]["extension"].asString(), ".csv");
    EXPECT_FALSE(source["enabled"].asBool());

    const auto rules = invoke([&](auto callback) {
        controller.get_qc_rules(request(), std::move(callback));
    });
    EXPECT_TRUE(body(rules)["data"]["items"][0]["config"].isObject());

    const auto tasks = invoke([&](auto callback) {
        controller.get_tasks(request({{"node_code", "node-http-025"}}),
                             std::move(callback));
    });
    EXPECT_EQ(body(tasks)["data"]["items"][0]["qc_rule_ids"].size(), 2U);
}

TEST(ManagementHttpControllerTest, MapsRunEvidenceFiltersAndSanitizesFailures) {
    auto handlers = empty_handlers();
    handlers.list_task_runs = [](const TaskRunListRequest&) {
        TaskRunRecord run;
        run.id = "51";
        run.task_id = "41";
        run.node_code = "node-http-025";
        run.status = labbridge::core::TaskRunStatus::Running;
        return ManagementPageResult<ManagementTaskRun>{
            labbridge::core::Status::success(),
            {{{{run, true, 3600}}}, std::nullopt, false}};
    };
    handlers.find_task_run = [](const std::string&, const std::string&) {
        TaskRunSummaryRecord summary;
        summary.task_run.id = "51";
        summary.task_run.task_id = "41";
        summary.task_run.node_code = "node-http-025";
        summary.task_run.status = labbridge::core::TaskRunStatus::Running;
        summary.raw_file_count = 1;
        summary.parsed_record_count = 2;
        summary.qc_result_count = 4;
        summary.alert_count = 1;
        return ManagementItemResult<ManagementTaskRunSummary>{
            labbridge::core::Status::success(),
            ManagementTaskRunSummary{summary, true, 3600}};
    };
    handlers.list_raw_files = [](const RunScopedListRequest&) {
        RawFileRecord file;
        file.id = "61";
        file.task_run_id = "51";
        file.node_code = "node-http-025";
        file.original_name = "observation.csv";
        file.storage_path = "/archive/observation.csv";
        file.size_bytes = 123;
        return ManagementPageResult<RawFileRecord>{
            labbridge::core::Status::success(), {{{file}}, std::nullopt, false}};
    };
    handlers.list_parsed_records = [](const RunScopedListRequest&) {
        ParsedRecordRecord parsed;
        parsed.id = "71";
        parsed.raw_file_id = "61";
        parsed.task_run_id = "51";
        parsed.record.station_code = "station-1";
        parsed.record.payload_json = R"({"temperature":23.5})";
        return ManagementPageResult<ParsedRecordRecord>{
            labbridge::core::Status::success(), {{{parsed}}, std::nullopt, false}};
    };
    handlers.list_qc_results = [](const QcResultListRequest&) {
        QcResultRecord result;
        result.id = "81";
        result.task_run_id = "51";
        result.parsed_record_id = "71";
        result.qc_rule_id = "31";
        result.result = "passed";
        return ManagementPageResult<QcResultRecord>{
            labbridge::core::Status::success(), {{{result}}, std::nullopt, false}};
    };
    handlers.list_alerts = [](const AlertListRequest&) {
        AlertRecord alert;
        alert.id = "91";
        alert.node_code = "node-http-025";
        alert.task_run_id = "51";
        alert.alert_type = "qc_failed";
        alert.severity = "warning";
        return ManagementPageResult<AlertRecord>{
            labbridge::core::Status::success(), {{{alert}}, std::nullopt, false}};
    };
    ManagementHttpController controller{std::move(handlers), empty_command_handlers()};

    const auto runs = invoke([&](auto callback) {
        controller.get_task_runs(request({{"node_code", "node-http-025"},
                                           {"task_id", "41"},
                                           {"status", "running"}}),
                                  std::move(callback));
    });
    EXPECT_TRUE(body(runs)["data"]["items"][0]["stale"].asBool());

    const auto detail = invoke([&](auto callback) {
        controller.get_task_run(request({{"node_code", "node-http-025"}}),
                                "51", std::move(callback));
    });
    EXPECT_EQ(body(detail)["data"]["qc_result_count"].asInt(), 4);

    const auto raw = invoke([&](auto callback) {
        controller.get_raw_files(request({{"task_run_id", "51"}}),
                                 std::move(callback));
    });
    EXPECT_EQ(body(raw)["data"]["items"][0]["size_bytes"].asInt64(), 123);

    const auto parsed = invoke([&](auto callback) {
        controller.get_parsed_records(request({{"task_run_id", "51"}}),
                                      std::move(callback));
    });
    EXPECT_DOUBLE_EQ(
        body(parsed)["data"]["items"][0]["payload"]["temperature"].asDouble(),
        23.5);

    EXPECT_EQ(invoke([&](auto callback) {
        controller.get_qc_results(request({{"task_run_id", "51"},
                                            {"result", "passed"}}),
                                  std::move(callback));
    })->statusCode(), drogon::k200OK);
    EXPECT_EQ(invoke([&](auto callback) {
        controller.get_alerts(request({{"node_code", "node-http-025"},
                                       {"task_run_id", "51"},
                                       {"status", "open"},
                                       {"severity", "warning"}}),
                              std::move(callback));
    })->statusCode(), drogon::k200OK);

    auto throwing_handlers = empty_handlers();
    throwing_handlers.list_nodes = [](const NodeListRequest&)
        -> ManagementPageResult<ManagementNode> {
        throw std::runtime_error("password=secret SQL SELECT private_payload");
    };
    ManagementHttpController throwing{std::move(throwing_handlers), empty_command_handlers()};
    const auto failure = invoke([&](auto callback) {
        throwing.get_nodes(request(), std::move(callback));
    });
    expect_error(failure, drogon::k500InternalServerError, "internal_error");
    EXPECT_EQ(body(failure)["error"]["message"].asString(),
              "internal server error");
}

TEST(ManagementHttpControllerTest, ParsesCommandsAndReturnsWrittenObjects) {
    auto commands = empty_command_handlers();
    ManagementDataSourceCreateRequest captured_source;
    ManagementQcRuleCreateRequest captured_rule;
    ManagementTaskCreateRequest captured_task;
    bool patched_enabled = true;

    commands.create_data_source = [&captured_source](
        const ManagementDataSourceCreateRequest& request) {
        captured_source = request;
        DataSourceRecord record;
        record.id = "101";
        record.node_code = request.node_code;
        record.source_type = request.source_type;
        record.name = request.name;
        record.config_json = request.config_json;
        record.enabled = request.enabled;
        record.created_at = "2026-08-21T01:02:03.000000Z";
        record.updated_at = record.created_at;
        return ManagementCommandResult{
            labbridge::core::Status::success(), record.id, record};
    };
    commands.create_qc_rule = [&captured_rule](
        const ManagementQcRuleCreateRequest& request) {
        captured_rule = request;
        QcRuleRecord record;
        record.id = "201";
        record.name = request.name;
        record.rule_type = request.rule_type;
        record.rule_config_json = request.rule_config_json;
        record.enabled = request.enabled;
        return ManagementCommandResult{
            labbridge::core::Status::success(), record.id, record};
    };
    commands.create_task = [&captured_task](
        const ManagementTaskCreateRequest& request) {
        captured_task = request;
        TaskRecord record;
        record.id = "301";
        record.node_code = request.node_code;
        record.data_source_id = request.data_source_id;
        record.name = request.name;
        record.task_type = request.task_type;
        record.schedule_expr = request.schedule_expr;
        record.parser_type = request.parser_type;
        record.qc_profile = request.qc_profile;
        record.qc_rule_ids = request.qc_rule_ids;
        record.enabled = request.enabled;
        return ManagementCommandResult{
            labbridge::core::Status::success(), record.id, record};
    };
    commands.set_task_enabled = [&patched_enabled](
        const std::string& task_id, bool enabled) {
        patched_enabled = enabled;
        TaskRecord record;
        record.id = task_id;
        record.node_code = "node-http-025";
        record.data_source_id = "101";
        record.name = "Minute CSV import";
        record.task_type = "local_file_import";
        record.schedule_expr = "*/5 * * * *";
        record.parser_type = "csv_observation";
        record.qc_profile = "default";
        record.qc_rule_ids = {"201", "202"};
        record.enabled = enabled;
        return ManagementCommandResult{
            labbridge::core::Status::success(), record.id, record};
    };
    ManagementHttpController controller{
        empty_handlers(), std::move(commands)};

    Json::Value source_body;
    source_body["node_code"] = "node-http-025";
    source_body["source_type"] = "local_directory";
    source_body["name"] = "Instrument inbox";
    source_body["config"]["root_path"] = "/srv/labbridge/inbox";
    source_body["config"]["extension"] = ".csv";
    source_body["enabled"] = true;
    const auto source_response = invoke([&](auto callback) {
        controller.post_data_source(
            json_request(drogon::Post, source_body), std::move(callback));
    });
    EXPECT_EQ(source_response->statusCode(), drogon::k201Created);
    EXPECT_EQ(captured_source.node_code, "node-http-025");
    EXPECT_EQ(body(source_response)["data"]["id"].asString(), "101");
    EXPECT_TRUE(body(source_response)["data"]["config"].isObject());
    EXPECT_TRUE(body(source_response)["data"]["created_at"].isString());

    Json::Value rule_body;
    rule_body["name"] = "Required fields";
    rule_body["rule_type"] = "required_fields";
    rule_body["config"] = Json::Value{Json::objectValue};
    rule_body["enabled"] = true;
    const auto rule_response = invoke([&](auto callback) {
        controller.post_qc_rule(
            json_request(drogon::Post, rule_body), std::move(callback));
    });
    EXPECT_EQ(rule_response->statusCode(), drogon::k201Created);
    EXPECT_EQ(captured_rule.rule_type, "required_fields");
    EXPECT_TRUE(body(rule_response)["data"]["config"].isObject());

    Json::Value task_body;
    task_body["node_code"] = "node-http-025";
    task_body["data_source_id"] = "101";
    task_body["name"] = "Minute CSV import";
    task_body["task_type"] = "local_file_import";
    task_body["schedule_expr"] = "*/5 * * * *";
    task_body["parser_type"] = "csv_observation";
    task_body["qc_profile"] = "default";
    task_body["qc_rule_ids"] = Json::Value{Json::arrayValue};
    task_body["qc_rule_ids"].append("202");
    task_body["qc_rule_ids"].append("201");
    task_body["enabled"] = true;
    const auto task_response = invoke([&](auto callback) {
        controller.post_task(
            json_request(drogon::Post, task_body), std::move(callback));
    });
    EXPECT_EQ(task_response->statusCode(), drogon::k201Created);
    EXPECT_EQ(captured_task.qc_rule_ids,
              (std::vector<std::string>{"202", "201"}));
    EXPECT_EQ(body(task_response)["data"]["qc_rule_ids"][0].asString(),
              "202");

    Json::Value patch_body;
    patch_body["enabled"] = false;
    const auto patch_response = invoke([&](auto callback) {
        controller.patch_task(
            json_request(drogon::Patch, patch_body), "301",
            std::move(callback));
    });
    EXPECT_EQ(patch_response->statusCode(), drogon::k200OK);
    EXPECT_FALSE(patched_enabled);
    EXPECT_FALSE(body(patch_response)["data"]["enabled"].asBool());
}

TEST(ManagementHttpControllerTest, RejectsInvalidCommandHttpBoundaries) {
    auto commands = empty_command_handlers();
    commands.create_data_source = [](const ManagementDataSourceCreateRequest& request) {
        if (request.source_type !=
            labbridge::core::SourceType::LocalDirectory) {
            return ManagementCommandResult{
                labbridge::core::Status::failure(
                    labbridge::core::StatusCode::InvalidArgument,
                    "source_type must be local_directory"),
                {}, {}};
        }
        return ManagementCommandResult{
            labbridge::core::Status::failure(
                labbridge::core::StatusCode::NotFound,
                "node is not found"),
            {}, {}};
    };
    commands.create_qc_rule = [](const ManagementQcRuleCreateRequest&) {
        return ManagementCommandResult{
            labbridge::core::Status::failure(
                labbridge::core::StatusCode::Conflict,
                "QC rule is disabled"),
            {}, {}};
    };
    commands.create_task = [](const ManagementTaskCreateRequest& request)
        -> ManagementCommandResult {
        if (request.qc_rule_ids.size() == 2U &&
            request.qc_rule_ids[0] == request.qc_rule_ids[1]) {
            return ManagementCommandResult{
                labbridge::core::Status::failure(
                    labbridge::core::StatusCode::InvalidArgument,
                    "qc_rule_ids must not contain duplicates"),
                {}, {}};
        }
        throw std::runtime_error(
            "password=secret SQL INSERT private_config");
    };
    commands.set_task_enabled = [](const std::string&, bool) {
        return ManagementCommandResult{
            labbridge::core::Status::failure(
                labbridge::core::StatusCode::InvalidArgument,
                "task_id must be a positive integer"),
            {}, {}};
    };
    ManagementHttpController controller{
        empty_handlers(), std::move(commands)};

    Json::Value source_body;
    source_body["node_code"] = "missing-node";
    source_body["source_type"] = "local_directory";
    source_body["name"] = "Instrument inbox";
    source_body["config"]["root_path"] = "/srv/labbridge/inbox";
    source_body["config"]["extension"] = ".csv";
    source_body["enabled"] = true;
    expect_error(invoke([&](auto callback) {
        controller.post_data_source(
            json_request(drogon::Post, source_body), std::move(callback));
    }), drogon::k404NotFound, "not_found");
    expect_error(invoke([&](auto callback) {
        controller.post_data_source(
            json_request(drogon::Post, source_body, false),
            std::move(callback));
    }), drogon::k415UnsupportedMediaType, "unsupported_media_type");

    source_body["source_type"] = "ftp";
    expect_error(invoke([&](auto callback) {
        controller.post_data_source(
            json_request(drogon::Post, source_body), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    source_body["source_type"] = "unsupported";
    expect_error(invoke([&](auto callback) {
        controller.post_data_source(
            json_request(drogon::Post, source_body), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    source_body["source_type"] = "local_directory";

    source_body["unexpected"] = true;
    expect_error(invoke([&](auto callback) {
        controller.post_data_source(
            json_request(drogon::Post, source_body), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    source_body.removeMember("unexpected");
    source_body["config"] = Json::Value{Json::arrayValue};
    expect_error(invoke([&](auto callback) {
        controller.post_data_source(
            json_request(drogon::Post, source_body), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");

    Json::Value rule_body;
    rule_body["name"] = "Disabled rule";
    rule_body["rule_type"] = "required_fields";
    rule_body["config"] = Json::Value{Json::objectValue};
    rule_body["enabled"] = false;
    expect_error(invoke([&](auto callback) {
        controller.post_qc_rule(
            json_request(drogon::Post, rule_body), std::move(callback));
    }), drogon::k409Conflict, "conflict");

    Json::Value task_body;
    task_body["node_code"] = "node-http-025";
    task_body["data_source_id"] = "101";
    task_body["name"] = "Minute CSV import";
    task_body["task_type"] = "local_file_import";
    task_body["schedule_expr"] = "*/5 * * * *";
    task_body["parser_type"] = "csv_observation";
    task_body["qc_profile"] = "default";
    task_body["qc_rule_ids"] = Json::Value{Json::arrayValue};
    task_body["qc_rule_ids"].append(201);
    task_body["enabled"] = true;
    expect_error(invoke([&](auto callback) {
        controller.post_task(
            json_request(drogon::Post, task_body), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    task_body["qc_rule_ids"][0] = "201";
    task_body["qc_rule_ids"].append("201");
    expect_error(invoke([&](auto callback) {
        controller.post_task(
            json_request(drogon::Post, task_body), std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    task_body["qc_rule_ids"][1] = "202";
    const auto internal = invoke([&](auto callback) {
        controller.post_task(
            json_request(drogon::Post, task_body), std::move(callback));
    });
    expect_error(internal, drogon::k500InternalServerError, "internal_error");
    EXPECT_EQ(body(internal)["error"]["message"].asString(),
              "internal server error");

    Json::Value patch_body;
    patch_body["enabled"] = false;
    patch_body["name"] = "not allowed";
    expect_error(invoke([&](auto callback) {
        controller.patch_task(
            json_request(drogon::Patch, patch_body), "0",
            std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
    patch_body.removeMember("name");
    expect_error(invoke([&](auto callback) {
        controller.patch_task(
            json_request(drogon::Patch, patch_body), "0",
            std::move(callback));
    }), drogon::k400BadRequest, "invalid_argument");
}

}  // namespace
