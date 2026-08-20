#include "labbridge/server/http/management_http_controller.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

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

drogon::HttpRequestPtr request(
    std::initializer_list<std::pair<std::string, std::string>> parameters = {}) {
    auto value = drogon::HttpRequest::newHttpRequest();
    value->setMethod(drogon::Get);
    for (const auto& parameter : parameters) {
        value->setParameter(parameter.first, parameter.second);
    }
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
    ManagementHttpController controller{std::move(handlers)};

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
    ManagementHttpController controller{std::move(handlers)};

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
    ManagementHttpController controller{std::move(handlers)};

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
    ManagementHttpController throwing{std::move(throwing_handlers)};
    const auto failure = invoke([&](auto callback) {
        throwing.get_nodes(request(), std::move(callback));
    });
    expect_error(failure, drogon::k500InternalServerError, "internal_error");
    EXPECT_EQ(body(failure)["error"]["message"].asString(),
              "internal server error");
}

}  // namespace
