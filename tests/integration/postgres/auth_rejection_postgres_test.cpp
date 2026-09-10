// 真实库认证回验：被 401/403 拒绝的请求不允许产生任何业务写入。
// 通过拒绝前后各业务表的行数对比证明“拒绝请求无写入”。
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

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <json/writer.h>

#include <cstdlib>
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

std::map<std::string, long long> business_row_counts(LibpqSqlSession& session) {
    const auto rows = session.query_all(
        "SELECT 'nodes' AS entity, count(*)::bigint AS count FROM nodes "
        "UNION ALL SELECT 'data_sources', count(*) FROM data_sources "
        "UNION ALL SELECT 'tasks', count(*) FROM tasks "
        "UNION ALL SELECT 'task_runs', count(*) FROM task_runs "
        "UNION ALL SELECT 'raw_files', count(*) FROM raw_files "
        "UNION ALL SELECT 'parsed_records', count(*) FROM parsed_records "
        "UNION ALL SELECT 'qc_results', count(*) FROM qc_results "
        "UNION ALL SELECT 'alerts', count(*) FROM alerts "
        "UNION ALL SELECT 'agent_report_receipts', count(*) "
        "  FROM agent_report_receipts",
        {});
    std::map<std::string, long long> counts;
    for (const auto& row : rows) {
        counts[value_or_empty(row, "entity")] =
            std::stoll(value_or_empty(row, "count"));
    }
    return counts;
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
        report_controller_ = std::make_unique<AgentReportHttpController>(
            authenticator,
            [this](const RawFileManifestRequest& request) {
                return report_executor_->accept_raw_file_manifest(request);
            },
            [this](const TaskRunReportRequest& request) {
                return report_executor_->accept_task_run_report(request);
            });

        auto task_run_executor =
            std::make_shared<PostgresTaskRunExecutor>(connection_info_);
        task_run_controller_ = std::make_unique<TaskRunHttpController>(
            authenticator,
            [task_run_executor](const StartTaskRunRequest& request) {
                return task_run_executor->start(request);
            });

        auto control_executor =
            std::make_shared<PostgresAgentControlExecutor>(connection_info_);
        control_controller_ = std::make_unique<AgentControlHttpController>(
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
        // 其余 handler 用 404 桩占位：本测试只验证拒绝请求无写入。
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
        management_controller_ = std::make_unique<ManagementHttpController>(
            authenticator, std::move(query_handlers),
            std::move(command_handlers));

        before_ = business_row_counts(*session_);
    }

    void TearDown() override {
        if (!session_) {
            return;
        }
        try {
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

    // 与既有 HTTP 测试一致的调用方式：operation 接收一个响应回调并向
    // controller 转发（std::move 避免拷贝），捕获到的响应作为结果返回。
    template <typename Operation>
    static drogon::HttpResponsePtr invoke(Operation operation) {
        drogon::HttpResponsePtr response;
        operation([&response](const drogon::HttpResponsePtr& current) {
            response = current;
        });
        EXPECT_NE(response, nullptr);
        return response;
    }

    void expect_rejected(const drogon::HttpResponsePtr& response,
                         drogon::HttpStatusCode status,
                         const std::string& code) const {
        ASSERT_NE(response, nullptr);
        EXPECT_EQ(response->statusCode(), status);
        const auto& json = *response->getJsonObject();
        EXPECT_EQ(json["error"]["code"].asString(), code);
    }

    std::string connection_info_;
    std::unique_ptr<LibpqSqlSession> session_;
    std::string suffix_;
    std::shared_ptr<PostgresAgentReportExecutor> report_executor_;
    std::unique_ptr<AgentReportHttpController> report_controller_;
    std::unique_ptr<TaskRunHttpController> task_run_controller_;
    std::unique_ptr<AgentControlHttpController> control_controller_;
    std::unique_ptr<ManagementHttpController> management_controller_;
    std::map<std::string, long long> before_;
};

TEST_F(AuthRejectionPostgresTest, RejectedRequestsWriteNothing) {
    Json::Value register_body;
    register_body["node_code"] = kNodeA;
    register_body["name"] = "auth reject node a";
    register_body["agent_version"] = labbridge::core::kVersion;

    Json::Value heartbeat_body;
    heartbeat_body["node_code"] = kNodeA;
    heartbeat_body["agent_version"] = labbridge::core::kVersion;
    heartbeat_body["reported_at"] = "2026-09-10 08:00:00+08";

    Json::Value start_body;
    start_body["node_code"] = kNodeA;
    start_body["task_id"] = "1";
    start_body["execution_key"] = "auth-reject-key";
    start_body["scheduled_for"] = "2026-09-10T00:00:00Z";
    start_body["started_at"] = "2026-09-10T00:00:01Z";
    start_body["trigger_type"] = "scheduled";

    Json::Value manifest_body;
    manifest_body["task_run_id"] = "1";
    manifest_body["node_code"] = kNodeA;
    manifest_body["idempotency_key"] = "auth-reject-manifest";
    manifest_body["files"] = Json::Value{Json::arrayValue};

    Json::Value report_body;
    report_body["task_run_id"] = "1";
    report_body["node_code"] = kNodeA;
    report_body["idempotency_key"] = "auth-reject-report";
    report_body["status"] = "succeeded";
    report_body["parsed_records"] = Json::Value{Json::arrayValue};

    Json::Value source_body;
    source_body["node_code"] = kNodeA;
    source_body["source_type"] = "local_directory";
    source_body["name"] = "auth reject source";
    source_body["config"]["root_path"] = "/srv/inbox";
    source_body["config"]["extension"] = ".csv";
    source_body["enabled"] = true;

    struct RejectedCall {
        std::string name;
        drogon::HttpResponsePtr response;
    };

    // 各请求按场景携带不同凭据：构造即发，捕获响应。
    auto foreign_body = [](Json::Value body) {
        body["node_code"] = kNodeB;
        return write_json(body);
    };

    const std::vector<RejectedCall> rejections = {
        // 管理接口：无 token / 节点密钥 → 401。
        {"management list nodes without token",
         invoke([&](auto callback) {
             management_controller_->get_nodes(
                 agent_request(drogon::Get, "", "", ""), std::move(callback));
         })},
        {"management list nodes with agent token",
         invoke([&](auto callback) {
             management_controller_->get_nodes(
                 agent_request(drogon::Get, "", kTokenA, kNodeA),
                 std::move(callback));
         })},
        {"management create data source without token",
         invoke([&](auto callback) {
             management_controller_->post_data_source(
                 agent_request(drogon::Post, write_json(source_body), "", ""),
                 std::move(callback));
         })},

        // Agent 接口：无 token / 错 token / 冒充请求头 → 401。
        {"register without token",
         invoke([&](auto callback) {
             control_controller_->post_register(
                 agent_request(drogon::Post, write_json(register_body), "", ""),
                 std::move(callback));
         })},
        {"heartbeat with management token",
         invoke([&](auto callback) {
             control_controller_->post_heartbeat(
                 agent_request(drogon::Post, write_json(heartbeat_body),
                               kManagementToken, kNodeA),
                 std::move(callback));
         })},
        {"heartbeat with node b token",
         invoke([&](auto callback) {
             control_controller_->post_heartbeat(
                 agent_request(drogon::Post, write_json(heartbeat_body),
                               kTokenB, kNodeA),
                 std::move(callback));
         })},
        {"config for foreign node",
         invoke([&](auto callback) {
             control_controller_->get_config(
                 agent_request(drogon::Get, "", kTokenA, kNodeA), kNodeB,
                 std::move(callback));
         })},

        // start / manifest / report：无 token → 401；跨节点声明 → 403。
        {"start without token",
         invoke([&](auto callback) {
             task_run_controller_->post_start(
                 agent_request(drogon::Post, write_json(start_body), "", ""),
                 std::move(callback));
         })},
        {"start declaring foreign node",
         invoke([&](auto callback) {
             task_run_controller_->post_start(
                 agent_request(drogon::Post, foreign_body(start_body), kTokenA,
                               kNodeA),
                 std::move(callback));
         })},
        {"manifest without token",
         invoke([&](auto callback) {
             report_controller_->post_raw_file_manifest(
                 agent_request(drogon::Post, write_json(manifest_body), "", ""),
                 std::move(callback));
         })},
        {"manifest declaring foreign node",
         invoke([&](auto callback) {
             report_controller_->post_raw_file_manifest(
                 agent_request(drogon::Post, foreign_body(manifest_body),
                               kTokenA, kNodeA),
                 std::move(callback));
         })},
        {"report without token",
         invoke([&](auto callback) {
             report_controller_->post_task_run_report(
                 agent_request(drogon::Post, write_json(report_body), "", ""),
                 std::move(callback));
         })},
        {"report declaring foreign node",
         invoke([&](auto callback) {
             report_controller_->post_task_run_report(
                 agent_request(drogon::Post, foreign_body(report_body),
                               kTokenA, kNodeA),
                 std::move(callback));
         })},
    };

    for (const auto& rejection : rejections) {
        ASSERT_NE(rejection.response, nullptr) << rejection.name;
        const int status = static_cast<int>(rejection.response->statusCode());
        EXPECT_TRUE(status == 401 || status == 403) << rejection.name;
        const auto& json = *rejection.response->getJsonObject();
        const std::string code = json["error"]["code"].asString();
        EXPECT_TRUE(code == "unauthenticated" || code == "forbidden")
            << rejection.name;
        std::cout << "rejected " << rejection.name << " status=" << status
                  << " code=" << code << std::endl;
    }

    // 核心断言：所有业务表在拒绝前后行数一致，无任何写入。
    const auto after = business_row_counts(*session_);
    EXPECT_EQ(after, before_);

    std::cout << "auth_rejection rows_before=";
    for (const auto& [entity, count] : before_) {
        std::cout << entity << "=" << count << " ";
    }
    std::cout << std::endl;
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
