#include "labbridge/core/version.h"
#include "labbridge/server/http/agent_control_http_controller.h"
#include "labbridge/server/http/agent_report_http_controller.h"
#include "labbridge/server/http/http_authenticator.h"
#include "labbridge/server/http/management_http_controller.h"
#include "labbridge/server/http/task_run_http_controller.h"
#include "labbridge/server/postgres/agent_control_executor.h"
#include "labbridge/server/postgres/agent_report_executor.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/management_command_executor.h"
#include "labbridge/server/postgres/management_query_executor.h"
#include "labbridge/server/postgres/task_run_executor.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <json/writer.h>

#include <cstdlib>
#include <future>
#include <thread>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace labbridge::server;
using labbridge::server::storage::value_or_empty;

// 节点 A/B 独立密钥，覆盖冒充与越权两类拒绝。
const std::string kManagementToken(64, '1');
const std::string kNodeA = "auth-reject-node-a";
const std::string kNodeB = "auth-reject-node-b";
const std::string kTokenA(64, 'a');
const std::string kTokenB(64, 'b');

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

// 连字段值一起比较，心跳时间或任务状态被误改也能发现。
std::map<std::string, std::string> business_snapshot(LibpqSqlSession& session) {
    std::map<std::string, std::string> snapshot;
    for (const std::string table : {
             "nodes", "data_sources", "tasks", "task_runs", "raw_files",
             "parsed_records", "qc_rules", "qc_results", "alerts",
             "agent_report_receipts", "task_qc_rules"}) {
        const auto row = session.query_one(
            "SELECT COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), "
            "'[]'::jsonb)::text AS data FROM " + table + " t", {});
        snapshot[table] = value_or_empty(*row, "data");
    }
    return snapshot;
}

class AuthRejectionPostgresTest : public testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("LABBRIDGE_DATABASE_URL");
        ASSERT_NE(url, nullptr);
        ASSERT_FALSE(std::string(url).empty());
        connection_info_ = url;
        session_ = std::make_unique<LibpqSqlSession>(connection_info_);

        const auto transaction = session_->query_one(
            "SELECT txid_current()::text AS id", {});
        ASSERT_TRUE(transaction.has_value());
        suffix_ = value_or_empty(*transaction, "id");

        // 两条真实节点记录，供 agent 接口做归属检查。
        insert("INSERT INTO nodes (node_code,name,status,agent_version) "
               "VALUES ($1,'auth reject node a','online','0.1.0')", {kNodeA});
        insert("INSERT INTO nodes (node_code,name,status,agent_version) "
               "VALUES ($1,'auth reject node b','online','0.1.0')", {kNodeB});

        // 四组 controller 与真实 executor 相连，和 main.cpp 的装配一致。
        auto authenticator = std::make_shared<HttpAuthenticator>(
            HttpAuthenticator::CredentialSet{
                kManagementToken, {{kNodeA, kTokenA}, {kNodeB, kTokenB}}});

        report_executor_ =
            std::make_shared<PostgresAgentReportExecutor>(connection_info_);
        report_controller_ = std::make_shared<AgentReportHttpController>(
            authenticator,
            [this](const RawFileManifestRequest& request) {
                return report_executor_->accept_raw_file_manifest(request);
            },
            [this](const TaskRunReportRequest& request) {
                return report_executor_->accept_task_run_report(request);
            });

        auto task_run_executor =
            std::make_shared<PostgresTaskRunExecutor>(connection_info_);
        task_run_controller_ = std::make_shared<TaskRunHttpController>(
            authenticator,
            [task_run_executor](const StartTaskRunRequest& request) {
                return task_run_executor->start(request);
            });

        auto control_executor =
            std::make_shared<PostgresAgentControlExecutor>(connection_info_);
        control_controller_ = std::make_shared<AgentControlHttpController>(
            authenticator,
            [control_executor](const labbridge::core::NodeInfo& node) {
                return control_executor->register_node(node);
            },
            [control_executor](const labbridge::core::NodeHeartbeat& heartbeat) {
                return control_executor->accept_heartbeat(heartbeat);
            },
            [control_executor](const std::string& node_code) {
                return control_executor->find_config(node_code);
            });

        auto query_executor = std::make_shared<PostgresManagementQueryExecutor>(
            connection_info_, 600, 3600);
        auto command_executor =
            std::make_shared<PostgresManagementCommandExecutor>(
                connection_info_);
        ManagementQueryHandlers query_handlers;
        query_handlers.list_nodes =
            [query_executor](const NodeListRequest& request) {
                return query_executor->list_nodes(request);
            };
        query_handlers.find_node =
            [query_executor](const std::string& node_code) {
                return query_executor->find_node(node_code);
            };
        query_handlers.list_data_sources =
            [query_executor](const NodeScopedListRequest& request) {
                return query_executor->list_data_sources(request);
            };
        query_handlers.list_qc_rules =
            [query_executor](const QcRuleListRequest& request) {
                return query_executor->list_qc_rules(request);
            };
        query_handlers.list_tasks =
            [query_executor](const NodeScopedListRequest& request) {
                return query_executor->list_tasks(request);
            };
        query_handlers.list_task_runs =
            [query_executor](const TaskRunListRequest& request) {
                return query_executor->list_task_runs(request);
            };
        query_handlers.find_task_run =
            [query_executor](const std::string& node_code,
                             const std::string& task_run_id) {
                return query_executor->find_task_run(node_code, task_run_id);
            };
        query_handlers.list_raw_files =
            [query_executor](const RunScopedListRequest& request) {
                return query_executor->list_raw_files(request);
            };
        query_handlers.list_parsed_records =
            [query_executor](const RunScopedListRequest& request) {
                return query_executor->list_parsed_records(request);
            };
        query_handlers.list_qc_results =
            [query_executor](const QcResultListRequest& request) {
                return query_executor->list_qc_results(request);
            };
        query_handlers.list_alerts =
            [query_executor](const AlertListRequest& request) {
                return query_executor->list_alerts(request);
            };
        ManagementCommandHandlers command_handlers;
        command_handlers.create_data_source =
            [command_executor](const ManagementDataSourceCreateRequest& request) {
                return command_executor->create_data_source(request);
            };
        command_handlers.create_qc_rule =
            [command_executor](const ManagementQcRuleCreateRequest& request) {
                return command_executor->create_qc_rule(request);
            };
        command_handlers.create_task =
            [command_executor](const ManagementTaskCreateRequest& request) {
                return command_executor->create_task(request);
            };
        command_handlers.set_task_enabled =
            [command_executor](const std::string& task_id, bool enabled) {
                return command_executor->set_task_enabled(task_id, enabled);
            };
        management_controller_ = std::make_shared<ManagementHttpController>(
            authenticator, std::move(query_handlers),
            std::move(command_handlers));

        for (const auto& node : {kNodeA, kNodeB}) {
            const auto source = insert_id(
                "INSERT INTO data_sources (node_id,source_type,name,config_json,enabled) "
                "SELECT id,'local_directory','isolation source','{}',true "
                "FROM nodes WHERE node_code=$1 RETURNING id::text AS id", {node});
            const auto task = insert_id(
                "INSERT INTO tasks (node_id,data_source_id,name,task_type,schedule_expr,"
                "parser_type,enabled) SELECT id,$2::bigint,'isolation task',"
                "'local_file_import','* * * * *','csv_observation',true "
                "FROM nodes WHERE node_code=$1 RETURNING id::text AS id", {node, source});
            task_ids_[node] = task;
            run_ids_[node] = insert_id(
                "INSERT INTO task_runs (node_id,task_id,status,started_at,trigger_type) "
                "SELECT id,$2::bigint,'running',now(),'scheduled' "
                "FROM nodes WHERE node_code=$1 RETURNING id::text AS id", {node, task});
        }
        raw_id_ = insert_id(
            "INSERT INTO raw_files (node_id,task_run_id,original_name,storage_path,"
            "size_bytes,ingest_status) SELECT id,$2::bigint,'b.csv','/archive/b.csv',1,"
            "'archived' FROM nodes WHERE node_code=$1 RETURNING id::text AS id",
            {kNodeB, run_ids_.at(kNodeB)});

        // 使用生产路由注册，经过 listener 和请求分发后再进入 controller。
        auto& app = drogon::app();
        control_controller_->register_routes(app);
        task_run_controller_->register_routes(app);
        report_controller_->register_routes(app);
        management_controller_->register_routes(app);
        app.addListener("127.0.0.1", 0).setThreadNum(1);
        std::promise<unsigned short> ready;
        auto port = ready.get_future();
        app.registerBeginningAdvice([&app, &ready] {
            ready.set_value(app.getListeners().front().toPort());
        });
        server_thread_ = std::thread([&app] { app.run(); });
        client_ = drogon::HttpClient::newHttpClient(
            "http://127.0.0.1:" + std::to_string(port.get()));
    }

    void TearDown() override {
        if (server_thread_.joinable()) {
            drogon::app().quit();
            server_thread_.join();
        }
        if (!session_) {
            return;
        }
        try {
            for (const std::string table : {"raw_files", "task_runs", "tasks", "data_sources"}) {
                session_->execute(
                    "DELETE FROM " + table + " WHERE node_id IN "
                    "(SELECT id FROM nodes WHERE node_code IN ($1,$2))", {kNodeA, kNodeB});
            }
            session_->execute(
                "DELETE FROM nodes WHERE node_code IN ($1,$2)",
                {kNodeA, kNodeB});
        } catch (const std::exception& error) {
            ADD_FAILURE() << "fixture cleanup failed: " << error.what();
        }
    }

    void insert(const std::string& sql, const SqlParams& params) {
        session_->execute(sql, params);
    }

    drogon::HttpRequestPtr agent_request(drogon::HttpMethod method,
                                         const std::string& body,
                                         const std::string& token,
                                         const std::string& header_node) const {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(method);
        if (!body.empty()) {
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(body);
        }
        if (!token.empty()) {
            request->addHeader("Authorization", "Bearer " + token);
        }
        if (!header_node.empty()) {
            request->addHeader("X-LabBridge-Node-Code", header_node);
        }
        return request;
    }

    std::string insert_id(const std::string& sql, const SqlParams& params) {
        return value_or_empty(*session_->query_one(sql, params), "id");
    }

    void expect_rejected(const std::string& path,
                         const drogon::HttpRequestPtr& request,
                         int expected_status) {
        SCOPED_TRACE(path);
        const auto before = business_snapshot(*session_);
        request->setPath(path);
        const auto [result, response] = client_->sendRequest(request, 5.0);
        ASSERT_EQ(result, drogon::ReqResult::Ok);
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), expected_status);
        if (expected_status == 401) {
            EXPECT_EQ(response->getHeader("WWW-Authenticate"), "Bearer");
        }
        ASSERT_NE(response->getJsonObject(), nullptr);
        EXPECT_FALSE((*response->getJsonObject())["ok"].asBool());
        EXPECT_EQ(business_snapshot(*session_), before);
    }

    std::string connection_info_;
    std::unique_ptr<LibpqSqlSession> session_;
    std::string suffix_;
    std::shared_ptr<PostgresAgentReportExecutor> report_executor_;
    std::shared_ptr<AgentReportHttpController> report_controller_;
    std::shared_ptr<TaskRunHttpController> task_run_controller_;
    std::shared_ptr<AgentControlHttpController> control_controller_;
    std::shared_ptr<ManagementHttpController> management_controller_;
    std::map<std::string, std::string> task_ids_;
    std::map<std::string, std::string> run_ids_;
    std::string raw_id_;
    std::thread server_thread_;
    drogon::HttpClientPtr client_;
};

TEST_F(AuthRejectionPostgresTest, RejectedRoutesLeaveEveryBusinessRowUnchanged) {
    Json::Value registration;
    registration["node_code"] = kNodeB;
    registration["name"] = "attempted rename";
    registration["agent_version"] = "0.1.0";
    Json::Value heartbeat = registration;
    heartbeat["reported_at"] = "2026-09-11T00:00:00Z";
    Json::Value start;
    start["node_code"] = kNodeB;
    start["task_id"] = task_ids_.at(kNodeB);
    start["execution_key"] = "isolation-start-" + suffix_;
    start["scheduled_for"] = "2026-09-11T00:00:00Z";
    start["started_at"] = "2026-09-11T00:00:01Z";
    start["trigger_type"] = "scheduled";
    Json::Value manifest;
    manifest["node_code"] = kNodeB;
    manifest["task_run_id"] = run_ids_.at(kNodeB);
    manifest["idempotency_key"] = "isolation-manifest-" + suffix_;
    manifest["files"] = Json::Value{Json::arrayValue};
    Json::Value report;
    report["node_code"] = kNodeB;
    report["task_run_id"] = run_ids_.at(kNodeB);
    report["idempotency_key"] = "isolation-report-" + suffix_;
    report["status"] = "succeeded";
    report["finished_at"] = "2026-09-11T00:00:02Z";
    report["items_total"] = 0;
    report["items_success"] = 0;
    report["items_failed"] = 0;
    report["parsed_records"] = Json::Value{Json::arrayValue};

    struct Route {
        std::string path;
        drogon::HttpMethod method;
        Json::Value body;
    };
    const std::vector<Route> agents = {
        {"/api/v1/agents/register", drogon::Post, registration},
        {"/api/v1/agents/heartbeat", drogon::Post, heartbeat},
        {"/api/v1/agents/" + kNodeB + "/config", drogon::Get, {}},
        {"/api/v1/task-runs/start", drogon::Post, start},
        {"/api/v1/raw-files/manifest", drogon::Post, manifest},
        {"/api/v1/task-runs/report", drogon::Post, report},
    };
    for (const auto& route : agents) {
        // B 的请求头配 A 的密钥先被拒；A 身份通过后再检查 body/路径归属。
        for (const auto& token : {std::string{}, kTokenA, kManagementToken}) {
            expect_rejected(route.path, agent_request(
                route.method, write_json(route.body), token, kNodeB), 401);
        }
        expect_rejected(route.path, agent_request(
            route.method, write_json(route.body), kTokenA, kNodeA), 403);
    }

    // 身份与声明都是 A，资源却属于 B，交给原有业务归属校验拒绝。
    for (const auto& route : std::vector<Route>{agents[3], agents[4], agents[5]}) {
        auto body = route.body;
        body["node_code"] = kNodeA;
        expect_rejected(route.path, agent_request(
            route.method, write_json(body), kTokenA, kNodeA), 409);
    }
    report["node_code"] = kNodeA;
    report["task_run_id"] = run_ids_.at(kNodeA);
    Json::Value parsed;
    parsed["raw_file_id"] = raw_id_;
    parsed["station_code"] = "station-a";
    parsed["device_code"] = "device-a";
    parsed["record_time"] = "2026-09-11T00:00:00Z";
    parsed["payload_json"] = "{}";
    parsed["parse_status"] = "parsed";
    report["parsed_records"].append(parsed);
    report["items_total"] = 1;
    report["items_success"] = 1;
    expect_rejected("/api/v1/task-runs/report", agent_request(
        drogon::Post, write_json(report), kTokenA, kNodeA), 409);

    const std::vector<Route> management = {
        {"/api/v1/nodes", drogon::Get, {}},
        {"/api/v1/nodes/" + kNodeB, drogon::Get, {}},
        {"/api/v1/data-sources", drogon::Get, {}},
        {"/api/v1/qc-rules", drogon::Get, {}},
        {"/api/v1/tasks", drogon::Get, {}},
        {"/api/v1/task-runs", drogon::Get, {}},
        {"/api/v1/task-runs/" + run_ids_.at(kNodeB), drogon::Get, {}},
        {"/api/v1/raw-files", drogon::Get, {}},
        {"/api/v1/parsed-records", drogon::Get, {}},
        {"/api/v1/qc-results", drogon::Get, {}},
        {"/api/v1/alerts", drogon::Get, {}},
        {"/api/v1/data-sources", drogon::Post, {}},
        {"/api/v1/qc-rules", drogon::Post, {}},
        {"/api/v1/tasks", drogon::Post, {}},
        {"/api/v1/tasks/" + task_ids_.at(kNodeB), drogon::Patch, {}},
    };
    for (const auto& route : management) {
        for (const auto& token : {std::string{}, std::string(64, 'f'), kTokenA}) {
            for (const std::string body : {std::string{"{}"}, std::string{"{"}}) {
                expect_rejected(route.path, agent_request(
                    route.method, body, token, ""), 401);
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* connection = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection == nullptr || std::string{connection}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping "
                     "auth rejection PostgreSQL test\n";
        return 77;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
