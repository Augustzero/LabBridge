#include "support/server/in_memory_repositories.h"
#include "support/server/test_config_seed.h"
#include "labbridge/core/version.h"
#include "labbridge/server/http/agent_control_http_controller.h"
#include "labbridge/server/http/http_authenticator.h"
#include "labbridge/server/application/agent_control_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/writer.h>

#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

// Agent 接口凭据：节点 lab-node-http-control-019 的独立密钥。
const std::string kNodeCode = "lab-node-http-control-019";
const std::string kAgentToken(64, 'a');

std::shared_ptr<const labbridge::server::HttpAuthenticator> test_authenticator() {
    return std::make_shared<labbridge::server::HttpAuthenticator>(
        labbridge::server::HttpAuthenticator::CredentialSet{
            std::string(64, '1'), {{kNodeCode, kAgentToken}}});
}

void add_agent_credentials(drogon::HttpRequest& request,
                           const std::string& node_code = kNodeCode,
                           const std::string& token = kAgentToken) {
    request.addHeader("Authorization", "Bearer " + token);
    request.addHeader("X-LabBridge-Node-Code", node_code);
}

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

drogon::HttpResponsePtr invoke_register(
    const labbridge::server::AgentControlHttpController& controller,
    const std::string& body,
    bool json_content_type = true) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    if (json_content_type) {
        request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    request->setBody(body);
    add_agent_credentials(*request);

    drogon::HttpResponsePtr response;
    controller.post_register(
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

drogon::HttpResponsePtr invoke_heartbeat(
    const labbridge::server::AgentControlHttpController& controller,
    const Json::Value& body) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(write_json(body));
    add_agent_credentials(*request);

    drogon::HttpResponsePtr response;
    controller.post_heartbeat(
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

drogon::HttpResponsePtr invoke_config(
    const labbridge::server::AgentControlHttpController& controller,
    const std::string& node_code) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    add_agent_credentials(*request);
    drogon::HttpResponsePtr response;
    controller.get_config(
        request,
        node_code,
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

Json::Value registration_body(const std::string& node_code,
                              const std::string& name) {
    Json::Value body;
    body["node_code"] = node_code;
    body["name"] = name;
    body["agent_version"] = labbridge::core::kVersion;
    return body;
}

Json::Value heartbeat_body(const std::string& node_code) {
    Json::Value body;
    body["node_code"] = node_code;
    body["agent_version"] = labbridge::core::kVersion;
    body["reported_at"] = "2026-07-17 10:15:00+08";
    return body;
}

}  // namespace

TEST(AgentControlHttpControllerTest, MapsRegistrationHeartbeatConfigAndErrors) {
    labbridge::server::InMemoryNodeRepository node_repository;
    labbridge::server::InMemoryConfigRepository config_repository;
    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{config_repository};
    labbridge::server::AgentControlService agent_control_service{
        node_service,
        config_service};

    labbridge::server::AgentControlHttpController controller{
        test_authenticator(),
        [&agent_control_service](const labbridge::core::NodeInfo& node) {
            return agent_control_service.register_node(node);
        },
        [&agent_control_service](const labbridge::core::NodeHeartbeat& heartbeat) {
            return agent_control_service.accept_heartbeat(heartbeat);
        },
        [&agent_control_service](const std::string& node_code) {
            return agent_control_service.find_config(node_code);
        }};

    assert_error(
        invoke_register(controller, "{}", false),
        drogon::k415UnsupportedMediaType,
        "unsupported_media_type");
    assert_error(
        invoke_register(controller, "{"),
        drogon::k400BadRequest,
        "invalid_argument");

    auto wrong_type = registration_body("phase19-wrong-type", "wrong type");
    wrong_type["node_code"] = 19;
    assert_error(
        invoke_register(controller, write_json(wrong_type)),
        drogon::k400BadRequest,
        "invalid_argument");

    // 名称校验在认证节点自身的请求上触发；声明其他节点会先被 403 拦截。
    auto empty_name = registration_body(kNodeCode, "");
    assert_error(
        invoke_register(controller, write_json(empty_name)),
        drogon::k400BadRequest,
        "invalid_argument");

    const std::string node_code = "lab-node-http-control-019";
    const auto register_response = invoke_register(
        controller,
        write_json(registration_body(node_code, "phase19 HTTP node")));
    EXPECT_TRUE(register_response->statusCode() == drogon::k201Created);
    EXPECT_TRUE(response_json(register_response)["ok"].asBool());
    EXPECT_TRUE(response_json(register_response)["data"]["node_code"].asString() ==
           node_code);
    EXPECT_TRUE(response_json(register_response)["data"]["status"].asString() ==
           "offline");

    auto missing_reported_at = heartbeat_body(node_code);
    missing_reported_at.removeMember("reported_at");
    assert_error(
        invoke_heartbeat(controller, missing_reported_at),
        drogon::k400BadRequest,
        "invalid_argument");

    auto empty_reported_at = heartbeat_body(node_code);
    empty_reported_at["reported_at"] = "";
    assert_error(
        invoke_heartbeat(controller, empty_reported_at),
        drogon::k400BadRequest,
        "invalid_argument");

    // 凭据有效但 body 声明其他节点：HTTP 边界直接 403，不再进入业务层。
    assert_error(
        invoke_heartbeat(controller, heartbeat_body("phase19-missing-node")),
        drogon::k403Forbidden,
        "forbidden");

    const auto heartbeat_response =
        invoke_heartbeat(controller, heartbeat_body(node_code));
    EXPECT_TRUE(heartbeat_response->statusCode() == drogon::k200OK);
    EXPECT_TRUE(response_json(heartbeat_response)["data"]["status"].asString() ==
           "online");

    const auto data_source =
        labbridge::server::test_support::create_local_csv_data_source(
            config_repository, node_code, "phase19 local directory",
            R"({"path":"tests/fixtures/agent","pattern":"*.csv"})");
    EXPECT_FALSE(data_source.empty());

    const auto enabled_task =
        labbridge::server::test_support::create_csv_task(
            config_repository, node_code, data_source,
            "phase19 enabled task");
    EXPECT_FALSE(enabled_task.empty());

    const auto disabled_task =
        labbridge::server::test_support::create_csv_task(
            config_repository, node_code, data_source,
            "phase19 disabled task", false);
    EXPECT_FALSE(disabled_task.empty());

    // 路径声明的节点与认证节点不一致时，统一 403，不进入业务查询。
    assert_error(
        invoke_config(controller, ""),
        drogon::k403Forbidden,
        "forbidden");
    assert_error(
        invoke_config(controller, "phase19-missing-node"),
        drogon::k403Forbidden,
        "forbidden");

    const auto config_response = invoke_config(controller, node_code);
    EXPECT_TRUE(config_response->statusCode() == drogon::k200OK);
    const auto config_response_json = response_json(config_response);
    const auto& config = config_response_json["data"];
    EXPECT_TRUE(config["node"]["node_code"].asString() == node_code);
    EXPECT_TRUE(config["node"]["status"].asString() == "online");
    EXPECT_TRUE(config["node"]["agent_version"].asString() ==
           labbridge::core::kVersion);
    EXPECT_TRUE(config["tasks"].isArray());
    EXPECT_TRUE(config["tasks"].size() == 1);
    EXPECT_TRUE(config["tasks"][Json::ArrayIndex{0}]["id"].asString() ==
           enabled_task);
    EXPECT_TRUE(config["tasks"][Json::ArrayIndex{0}]["data_source_id"].asString() ==
           data_source);
    EXPECT_TRUE(config["tasks"][Json::ArrayIndex{0}]["enabled"].asBool());

    labbridge::server::AgentControlHttpController throwing_controller{
        test_authenticator(),
        [](const labbridge::core::NodeInfo&) -> labbridge::core::Status {
            throw std::runtime_error("database password must stay private");
        },
        [](const labbridge::core::NodeHeartbeat&) {
            return labbridge::core::Status::success();
        },
        [](const std::string&) {
            return labbridge::server::AgentConfigResult{
                labbridge::core::Status::success(), std::nullopt, {}};
        }};
    const auto internal_response = invoke_register(
        throwing_controller,
        write_json(registration_body(node_code, "phase19 throwing node")));
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
