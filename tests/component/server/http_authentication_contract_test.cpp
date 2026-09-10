// 全部 HTTP 路由的认证 contract：不认证进不了业务 handler，
// 管理 token 与节点密钥互不通用，节点只能声明自己的身份。
#include "labbridge/server/http/agent_control_http_controller.h"
#include "labbridge/server/http/agent_report_http_controller.h"
#include "labbridge/server/http/http_authenticator.h"
#include "labbridge/server/http/management_http_controller.h"
#include "labbridge/server/http/task_run_http_controller.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <json/writer.h>

#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace labbridge::server;

// 节点 A/B 各自独立密钥，覆盖节点隔离场景。
const std::string kManagementToken(64, '1');
const std::string kNodeA = "auth-node-a";
const std::string kNodeB = "auth-node-b";
const std::string kTokenA(64, 'a');
const std::string kTokenB(64, 'b');

std::shared_ptr<const HttpAuthenticator> test_authenticator() {
    return std::make_shared<HttpAuthenticator>(HttpAuthenticator::CredentialSet{
        kManagementToken, {{kNodeA, kTokenA}, {kNodeB, kTokenB}}});
}

// 调用 controller 入口并捕获响应；operation 只收一个响应回调。
template <typename Operation>
drogon::HttpResponsePtr send_response(Operation operation) {
    drogon::HttpResponsePtr response;
    operation([&response](const drogon::HttpResponsePtr& current) {
        response = current;
    });
    return response;
}

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

drogon::HttpRequestPtr make_request(drogon::HttpMethod method,
                                    const std::string& body = {}) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(method);
    if (!body.empty()) {
        request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        request->setBody(body);
    }
    return request;
}

void add_credentials(drogon::HttpRequest& request,
                     const std::string& token,
                     const std::string& node_code,
                     bool agent_headers) {
    if (!token.empty()) {
        request.addHeader("Authorization", "Bearer " + token);
    }
    if (agent_headers) {
        request.addHeader("X-LabBridge-Node-Code", node_code);
    }
}

void expect_unauthenticated(const drogon::HttpResponsePtr& response) {
    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k401Unauthorized);
    const auto& json = *response->getJsonObject();
    EXPECT_FALSE(json["ok"].asBool());
    EXPECT_EQ(json["error"]["code"].asString(), "unauthenticated");
    // 401 必须带 challenge 头。
    EXPECT_EQ(response->getHeader("WWW-Authenticate"), "Bearer");
}

void expect_forbidden(const drogon::HttpResponsePtr& response) {
    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k403Forbidden);
    const auto& json = *response->getJsonObject();
    EXPECT_EQ(json["error"]["code"].asString(), "forbidden");
}

// ---- 管理 controller：全部 route 的管理 token 保护 ----

struct ManagementCall {
    std::string route;
    // authorization 为完整的请求头取值；空串表示不携带认证头。
    std::function<drogon::HttpResponsePtr(const ManagementHttpController&,
                                          const std::string& authorization)>
        invoke;
};

std::vector<ManagementCall> management_calls() {
    Json::Value source_body;
    source_body["node_code"] = kNodeA;
    source_body["source_type"] = "local_directory";
    source_body["name"] = "auth source";
    source_body["config"]["root_path"] = "/srv/inbox";
    source_body["config"]["extension"] = ".csv";
    source_body["enabled"] = true;

    Json::Value rule_body;
    rule_body["name"] = "auth rule";
    rule_body["rule_type"] = "required_fields";
    rule_body["config"] = Json::Value{Json::objectValue};
    rule_body["enabled"] = true;

    Json::Value task_body;
    task_body["node_code"] = kNodeA;
    task_body["data_source_id"] = "11";
    task_body["name"] = "auth task";
    task_body["task_type"] = "local_file_import";
    task_body["schedule_expr"] = "*/5 * * * *";
    task_body["parser_type"] = "csv_observation";
    task_body["qc_profile"] = "default";
    task_body["qc_rule_ids"] = Json::Value{Json::arrayValue};
    task_body["enabled"] = true;

    Json::Value patch_body;
    patch_body["enabled"] = false;

    // 每条路由都给出能通过参数校验的最小请求，保证命中的是业务 handler。
    // lambda 会随 vector 返回，必须按值捕获 body，不能引用本函数局部变量。
    return {
        {"GET /nodes", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             return send_response([&](auto&& callback) {
                 controller.get_nodes(request, std::move(callback));
             });
         }},
        {"GET /nodes/{code}", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             return send_response([&](auto&& callback) {
                 controller.get_node(request, kNodeA, std::move(callback));
             });
         }},
        {"GET /data-sources", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             request->setParameter("node_code", kNodeA);
             return send_response([&](auto&& callback) {
                 controller.get_data_sources(request, std::move(callback));
             });
         }},
        {"GET /qc-rules", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             return send_response([&](auto&& callback) {
                 controller.get_qc_rules(request, std::move(callback));
             });
         }},
        {"GET /tasks", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             request->setParameter("node_code", kNodeA);
             return send_response([&](auto&& callback) {
                 controller.get_tasks(request, std::move(callback));
             });
         }},
        {"GET /task-runs", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             request->setParameter("node_code", kNodeA);
             return send_response([&](auto&& callback) {
                 controller.get_task_runs(request, std::move(callback));
             });
         }},
        {"GET /task-runs/{id}", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             request->setParameter("node_code", kNodeA);
             return send_response([&](auto&& callback) {
                 controller.get_task_run(request, "51", std::move(callback));
             });
         }},
        {"GET /raw-files", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             request->setParameter("task_run_id", "51");
             return send_response([&](auto&& callback) {
                 controller.get_raw_files(request, std::move(callback));
             });
         }},
        {"GET /parsed-records", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             request->setParameter("task_run_id", "51");
             return send_response([&](auto&& callback) {
                 controller.get_parsed_records(request, std::move(callback));
             });
         }},
        {"GET /qc-results", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             request->setParameter("task_run_id", "51");
             return send_response([&](auto&& callback) {
                 controller.get_qc_results(request, std::move(callback));
             });
         }},
        {"GET /alerts", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Get);
             if (!auth.empty()) request->addHeader("Authorization", auth);
             request->setParameter("node_code", kNodeA);
             return send_response([&](auto&& callback) {
                 controller.get_alerts(request, std::move(callback));
             });
         }},
        {"POST /data-sources", [=](auto& controller, const std::string& auth) {
             auto request =
                 make_request(drogon::Post, write_json(source_body));
             if (!auth.empty()) request->addHeader("Authorization", auth);
             return send_response([&](auto&& callback) {
                 controller.post_data_source(request, std::move(callback));
             });
         }},
        {"POST /qc-rules", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Post, write_json(rule_body));
             if (!auth.empty()) request->addHeader("Authorization", auth);
             return send_response([&](auto&& callback) {
                 controller.post_qc_rule(request, std::move(callback));
             });
         }},
        {"POST /tasks", [=](auto& controller, const std::string& auth) {
             auto request = make_request(drogon::Post, write_json(task_body));
             if (!auth.empty()) request->addHeader("Authorization", auth);
             return send_response([&](auto&& callback) {
                 controller.post_task(request, std::move(callback));
             });
         }},
        {"PATCH /tasks/{id}", [=](auto& controller, const std::string& auth) {
             auto request =
                 make_request(drogon::Patch, write_json(patch_body));
             if (!auth.empty()) request->addHeader("Authorization", auth);
             return send_response([&](auto&& callback) {
                 controller.patch_task(request, "41", std::move(callback));
             });
         }},
    };
}

TEST(HttpAuthenticationContractTest, ManagementRoutesRequireManagementToken) {
    int handler_calls = 0;
    auto count_call = [count = &handler_calls](auto result) {
        ++*count;
        return result;
    };

    ManagementQueryHandlers query_handlers{};
    query_handlers.list_nodes = [&count_call](const NodeListRequest&) {
        return count_call(ManagementPageResult<ManagementNode>{
            labbridge::core::Status::success(), {}});
    };
    query_handlers.find_node = [&count_call](const std::string&) {
        return count_call(ManagementItemResult<ManagementNodeSummary>{
            labbridge::core::Status::success(), std::nullopt});
    };
    query_handlers.list_data_sources = [&count_call](const NodeScopedListRequest&) {
        return count_call(ManagementPageResult<DataSourceRecord>{
            labbridge::core::Status::success(), {}});
    };
    query_handlers.list_qc_rules = [&count_call](const QcRuleListRequest&) {
        return count_call(ManagementPageResult<QcRuleRecord>{
            labbridge::core::Status::success(), {}});
    };
    query_handlers.list_tasks = [&count_call](const NodeScopedListRequest&) {
        return count_call(ManagementPageResult<TaskRecord>{
            labbridge::core::Status::success(), {}});
    };
    query_handlers.list_task_runs = [&count_call](const TaskRunListRequest&) {
        return count_call(ManagementPageResult<ManagementTaskRun>{
            labbridge::core::Status::success(), {}});
    };
    query_handlers.find_task_run = [&count_call](const std::string&,
                                                 const std::string&) {
        return count_call(ManagementItemResult<ManagementTaskRunSummary>{
            labbridge::core::Status::success(), std::nullopt});
    };
    query_handlers.list_raw_files = [&count_call](const RunScopedListRequest&) {
        return count_call(ManagementPageResult<RawFileRecord>{
            labbridge::core::Status::success(), {}});
    };
    query_handlers.list_parsed_records =
        [&count_call](const RunScopedListRequest&) {
            return count_call(ManagementPageResult<ParsedRecordRecord>{
                labbridge::core::Status::success(), {}});
        };
    query_handlers.list_qc_results = [&count_call](const QcResultListRequest&) {
        return count_call(ManagementPageResult<QcResultRecord>{
            labbridge::core::Status::success(), {}});
    };
    query_handlers.list_alerts = [&count_call](const AlertListRequest&) {
        return count_call(ManagementPageResult<AlertRecord>{
            labbridge::core::Status::success(), {}});
    };

    ManagementCommandHandlers command_handlers{};
    command_handlers.create_data_source =
        [&count_call](const ManagementDataSourceCreateRequest&) {
            return count_call(ManagementCommandResult{
                labbridge::core::Status::success(), "101", DataSourceRecord{}});
        };
    command_handlers.create_qc_rule =
        [&count_call](const ManagementQcRuleCreateRequest&) {
            return count_call(ManagementCommandResult{
                labbridge::core::Status::success(), "201", QcRuleRecord{}});
        };
    command_handlers.create_task =
        [&count_call](const ManagementTaskCreateRequest&) {
            return count_call(ManagementCommandResult{
                labbridge::core::Status::success(), "301", TaskRecord{}});
        };
    command_handlers.set_task_enabled =
        [&count_call](const std::string&, bool) {
            return count_call(ManagementCommandResult{
                labbridge::core::Status::success(), "301", TaskRecord{}});
        };

    const ManagementHttpController controller{
        test_authenticator(), std::move(query_handlers),
        std::move(command_handlers)};

    // 场景一：全部管理 route 用无 token / 错误 token / 节点密钥调用均为 401，
    // 且业务 handler 没有被调用；管理 token 能正常进入业务 handler。
    const std::vector<std::tuple<std::string, std::string>> rejected = {
        {"no token", ""},
        {"wrong token", "Bearer " + std::string(64, 'x')},
        // 节点密钥不能当管理凭据使用。
        {"agent token", "Bearer " + kTokenA},
        {"agent token without scheme", kTokenA},
    };
    for (const auto& call : management_calls()) {
        for (const auto& [name, authorization] : rejected) {
            SCOPED_TRACE(call.route + ": " + name);
            handler_calls = 0;
            const auto response = call.invoke(controller, authorization);
            expect_unauthenticated(response);
            EXPECT_EQ(handler_calls, 0) << "business handler must not run";
        }

        SCOPED_TRACE(call.route + ": management token");
        handler_calls = 0;
        const auto response =
            call.invoke(controller, "Bearer " + kManagementToken);
        EXPECT_EQ(handler_calls, 1) << "management token must reach handler";
    }
}

// ---- Agent controller：六个 Agent 接口的节点隔离 ----

class AgentRoutesAuthTest : public testing::Test {
protected:
    void SetUp() override {
        auto register_handler = [this](const labbridge::core::NodeInfo&) {
            ++register_calls_;
            return labbridge::core::Status::success();
        };
        auto heartbeat_handler = [this](const labbridge::core::NodeHeartbeat&) {
            ++heartbeat_calls_;
            return labbridge::core::Status::success();
        };
        auto config_handler = [this](const std::string&) {
            ++config_calls_;
            AgentConfigResult result;
            result.status = labbridge::core::Status::success();
            result.node = NodeRecord{
                {kNodeA, "node a", "0.1.0"},
                labbridge::core::NodeStatus::Online,
                "2026-09-10T00:00:00Z",
            };
            return result;
        };
        control_controller_ = std::make_unique<AgentControlHttpController>(
            test_authenticator(), register_handler, heartbeat_handler,
            config_handler);

        auto start_handler = [this](const StartTaskRunRequest&) {
            ++start_calls_;
            return TaskRunCreateResult{
                labbridge::core::Status::success(), "42", false};
        };
        task_run_controller_ = std::make_unique<TaskRunHttpController>(
            test_authenticator(), start_handler);

        auto manifest_handler = [this](const RawFileManifestRequest&) {
            ++manifest_calls_;
            return RawFileManifestResult{
                labbridge::core::Status::success(), {"51"}, false};
        };
        auto report_handler = [this](const TaskRunReportRequest&) {
            ++report_calls_;
            return TaskRunReportResult{
                labbridge::core::Status::success(), {"61"}, {"71"}, {"81"},
                false};
        };
        report_controller_ = std::make_unique<AgentReportHttpController>(
            test_authenticator(), manifest_handler, report_handler);
    }

    // handler 直接以模板转发，避免 std::function 把回调参数降级为左值。
    template <typename Handler>
    static drogon::HttpResponsePtr send(
        const drogon::HttpRequestPtr& request, Handler handler) {
        drogon::HttpResponsePtr response;
        handler(request,
                [&response](const drogon::HttpResponsePtr& current) {
                    response = current;
                });
        return response;
    }

    Json::Value register_body(const std::string& node_code) const {
        Json::Value body;
        body["node_code"] = node_code;
        body["name"] = "auth node";
        body["agent_version"] = "0.1.0";
        return body;
    }

    Json::Value heartbeat_body(const std::string& node_code) const {
        Json::Value body;
        body["node_code"] = node_code;
        body["agent_version"] = "0.1.0";
        body["reported_at"] = "2026-09-10 08:00:00+08";
        return body;
    }

    Json::Value start_body(const std::string& node_code) const {
        Json::Value body;
        body["node_code"] = node_code;
        body["task_id"] = "31";
        body["execution_key"] = "auth-key";
        body["scheduled_for"] = "2026-09-10T00:00:00Z";
        body["started_at"] = "2026-09-10T00:00:01Z";
        body["trigger_type"] = "scheduled";
        return body;
    }

    Json::Value manifest_body(const std::string& node_code) const {
        Json::Value body;
        body["task_run_id"] = "42";
        body["node_code"] = node_code;
        body["idempotency_key"] = "auth-manifest";
        body["files"] = Json::Value{Json::arrayValue};
        return body;
    }

    Json::Value report_body(const std::string& node_code) const {
        Json::Value body;
        body["task_run_id"] = "42";
        body["node_code"] = node_code;
        body["idempotency_key"] = "auth-report";
        body["status"] = "succeeded";
        body["parsed_records"] = Json::Value{Json::arrayValue};
        return body;
    }

    int register_calls_{0};
    int heartbeat_calls_{0};
    int config_calls_{0};
    int start_calls_{0};
    int manifest_calls_{0};
    int report_calls_{0};
    std::unique_ptr<AgentControlHttpController> control_controller_;
    std::unique_ptr<TaskRunHttpController> task_run_controller_;
    std::unique_ptr<AgentReportHttpController> report_controller_;
};

// 无 token / 错误 token / 管理 token / A 密钥冒充 B 请求头：
// 六个 Agent 接口都必须 401 且业务 handler 未被调用。
TEST_F(AgentRoutesAuthTest, AgentRoutesRejectInvalidCredentials) {
    struct AgentCall {
        std::string route;
        std::function<drogon::HttpResponsePtr(const std::string&,
                                              const std::string&)>
            invoke;
        int* handler_calls;
    };

    AgentCall calls[] = {
        {"register",
         [this](const std::string& token, const std::string& header_node) {
             auto request =
                 make_request(drogon::Post, write_json(register_body(kNodeA)));
             add_credentials(*request, token, header_node, true);
             return send(request, [this](auto&&... args) {
                 control_controller_->post_register(args...);
             });
         },
         &register_calls_},
        {"heartbeat",
         [this](const std::string& token, const std::string& header_node) {
             auto request = make_request(drogon::Post,
                                         write_json(heartbeat_body(kNodeA)));
             add_credentials(*request, token, header_node, true);
             return send(request, [this](auto&&... args) {
                 control_controller_->post_heartbeat(args...);
             });
         },
         &heartbeat_calls_},
        {"config",
         [this](const std::string& token, const std::string& header_node) {
             auto request = make_request(drogon::Get);
             add_credentials(*request, token, header_node, true);
             return send(request,
                         [this, request](auto&&, auto&& callback) {
                             control_controller_->get_config(
                                 request, kNodeA,
                                 std::forward<decltype(callback)>(callback));
                         });
         },
         &config_calls_},
        {"start",
         [this](const std::string& token, const std::string& header_node) {
             auto request =
                 make_request(drogon::Post, write_json(start_body(kNodeA)));
             add_credentials(*request, token, header_node, true);
             return send(request, [this](auto&&... args) {
                 task_run_controller_->post_start(args...);
             });
         },
         &start_calls_},
        {"manifest",
         [this](const std::string& token, const std::string& header_node) {
             auto request = make_request(drogon::Post,
                                         write_json(manifest_body(kNodeA)));
             add_credentials(*request, token, header_node, true);
             return send(request, [this](auto&&... args) {
                 report_controller_->post_raw_file_manifest(args...);
             });
         },
         &manifest_calls_},
        {"report",
         [this](const std::string& token, const std::string& header_node) {
             auto request =
                 make_request(drogon::Post, write_json(report_body(kNodeA)));
             add_credentials(*request, token, header_node, true);
             return send(request, [this](auto&&... args) {
                 report_controller_->post_task_run_report(args...);
             });
         },
         &report_calls_},
    };

    const std::vector<std::tuple<std::string, std::string, std::string>>
        rejected_credentials = {
            {"no token", "", ""},
            {"wrong token", std::string(64, 'x'), kNodeA},
            {"management token", kManagementToken, kNodeA},
            // A 的密钥冒充 B 的请求头：按 B 查密钥后比对失败。
            {"node a token as node b", kTokenA, kNodeB},
            {"node b token without header", kTokenB, ""},
        };
    for (const auto& call : calls) {
        for (const auto& [name, token, header_node] : rejected_credentials) {
            SCOPED_TRACE(call.route + ": " + name);
            *call.handler_calls = 0;
            const auto response = call.invoke(token, header_node);
            expect_unauthenticated(response);
            EXPECT_EQ(*call.handler_calls, 0)
                << "business handler must not run";
        }
    }
}

// A 的有效凭据 + 声明节点 B：六个接口都必须 403 且业务 handler 未被调用。
TEST_F(AgentRoutesAuthTest, AgentRoutesRejectForeignDeclaredNode) {
    // body 声明节点 B，凭据是节点 A 的。
    auto request_with = [this](auto make_body) {
        auto request =
            make_request(drogon::Post, write_json(make_body(kNodeB)));
        add_credentials(*request, kTokenA, kNodeA, true);
        return request;
    };
    auto config_request = make_request(drogon::Get);
    add_credentials(*config_request, kTokenA, kNodeA, true);

    struct AgentCall {
        std::string route;
        std::function<drogon::HttpResponsePtr()> invoke;
        int* handler_calls;
    };

    AgentCall calls[] = {
        {"register",
         [&] {
             return send(request_with([&](const std::string& node) {
                             return register_body(node);
                         }),
                         [this](auto&&... args) {
                             control_controller_->post_register(args...);
                         });
         },
         &register_calls_},
        {"heartbeat",
         [&] {
             return send(request_with([&](const std::string& node) {
                             return heartbeat_body(node);
                         }),
                         [this](auto&&... args) {
                             control_controller_->post_heartbeat(args...);
                         });
         },
         &heartbeat_calls_},
        {"config",
         [&] {
             return send(config_request,
                         [this, &config_request](auto&&, auto&& callback) {
                             control_controller_->get_config(
                                 config_request, kNodeB,
                                 std::forward<decltype(callback)>(callback));
                         });
         },
         &config_calls_},
        {"start",
         [&] {
             return send(request_with([&](const std::string& node) {
                             return start_body(node);
                         }),
                         [this](auto&&... args) {
                             task_run_controller_->post_start(args...);
                         });
         },
         &start_calls_},
        {"manifest",
         [&] {
             return send(request_with([&](const std::string& node) {
                             return manifest_body(node);
                         }),
                         [this](auto&&... args) {
                             report_controller_->post_raw_file_manifest(args...);
                         });
         },
         &manifest_calls_},
        {"report",
         [&] {
             return send(request_with([&](const std::string& node) {
                             return report_body(node);
                         }),
                         [this](auto&&... args) {
                             report_controller_->post_task_run_report(args...);
                         });
         },
         &report_calls_},
    };

    for (const auto& call : calls) {
        SCOPED_TRACE(call.route);
        *call.handler_calls = 0;
        const auto response = call.invoke();
        expect_forbidden(response);
        EXPECT_EQ(*call.handler_calls, 0) << "business handler must not run";
    }
}

// 节点 A 使用自己的凭据并声明自己：六个接口都应进入业务 handler。
TEST_F(AgentRoutesAuthTest, AgentRoutesAcceptOwnNodeCredentials) {
    auto own_request = [this](auto make_body, drogon::HttpMethod method) {
        auto request = make_request(method, write_json(make_body(kNodeA)));
        add_credentials(*request, kTokenA, kNodeA, true);
        return request;
    };

    EXPECT_EQ(send(own_request([&](const std::string& node) {
                       return register_body(node);
                   }, drogon::Post),
                   [this](auto&&... args) {
                       control_controller_->post_register(args...);
                   })->statusCode(), drogon::k201Created);
    EXPECT_EQ(register_calls_, 1);

    EXPECT_EQ(send(own_request([&](const std::string& node) {
                       return heartbeat_body(node);
                   }, drogon::Post),
                   [this](auto&&... args) {
                       control_controller_->post_heartbeat(args...);
                   })->statusCode(), drogon::k200OK);
    EXPECT_EQ(heartbeat_calls_, 1);

    auto own_config = make_request(drogon::Get);
    add_credentials(*own_config, kTokenA, kNodeA, true);
    EXPECT_EQ(send(own_config,
                   [this, own_config](auto&&, auto&& callback) {
                       control_controller_->get_config(
                           own_config, kNodeA,
                           std::forward<decltype(callback)>(callback));
                   })->statusCode(), drogon::k200OK);
    EXPECT_EQ(config_calls_, 1);

    EXPECT_EQ(send(own_request([&](const std::string& node) {
                       return start_body(node);
                   }, drogon::Post),
                   [this](auto&&... args) {
                       task_run_controller_->post_start(args...);
                   })->statusCode(), drogon::k201Created);
    EXPECT_EQ(start_calls_, 1);

    EXPECT_EQ(send(own_request([&](const std::string& node) {
                       return manifest_body(node);
                   }, drogon::Post),
                   [this](auto&&... args) {
                       report_controller_->post_raw_file_manifest(args...);
                   })->statusCode(), drogon::k201Created);
    EXPECT_EQ(manifest_calls_, 1);

    EXPECT_EQ(send(own_request([&](const std::string& node) {
                       return report_body(node);
                   }, drogon::Post),
                   [this](auto&&... args) {
                       report_controller_->post_task_run_report(args...);
                   })->statusCode(), drogon::k200OK);
    EXPECT_EQ(report_calls_, 1);
}

}  // namespace
