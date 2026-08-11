#include "support/server/in_memory_repositories.h"
#include "labbridge/core/version.h"
#include "labbridge/server/http/agent_control_http_controller.h"
#include "labbridge/server/application/agent_control_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"

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
    drogon::HttpResponsePtr response;
    controller.get_config(
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
    labbridge::server::ConfigService config_service{
        node_repository,
        config_repository};
    labbridge::server::AgentControlService agent_control_service{
        node_service,
        config_service};

    labbridge::server::AgentControlHttpController controller{
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

    auto empty_name = registration_body("phase19-empty-name", "");
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

    assert_error(
        invoke_heartbeat(controller, heartbeat_body("phase19-missing-node")),
        drogon::k404NotFound,
        "not_found");

    const auto heartbeat_response =
        invoke_heartbeat(controller, heartbeat_body(node_code));
    EXPECT_TRUE(heartbeat_response->statusCode() == drogon::k200OK);
    EXPECT_TRUE(response_json(heartbeat_response)["data"]["status"].asString() ==
           "online");

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase19 local directory",
        R"({"path":"tests/fixtures/agent","pattern":"*.csv"})",
        true,
    });
    EXPECT_TRUE(data_source.status.ok);

    const auto enabled_task = config_service.create_task({
        node_code,
        data_source.id,
        "phase19 enabled task",
        "collect_parse_qc",
        "*/5 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    EXPECT_TRUE(enabled_task.status.ok);

    const auto disabled_task = config_service.create_task({
        node_code,
        data_source.id,
        "phase19 disabled task",
        "collect_parse_qc",
        "* * * * *",
        "csv_observation",
        "basic",
        false,
    });
    EXPECT_TRUE(disabled_task.status.ok);

    assert_error(
        invoke_config(controller, ""),
        drogon::k400BadRequest,
        "invalid_argument");
    assert_error(
        invoke_config(controller, "phase19-missing-node"),
        drogon::k404NotFound,
        "not_found");

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
           enabled_task.id);
    EXPECT_TRUE(config["tasks"][Json::ArrayIndex{0}]["data_source_id"].asString() ==
           data_source.id);
    EXPECT_TRUE(config["tasks"][Json::ArrayIndex{0}]["enabled"].asBool());

    labbridge::server::AgentControlHttpController throwing_controller{
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
