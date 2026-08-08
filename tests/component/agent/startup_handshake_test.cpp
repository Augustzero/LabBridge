#include "labbridge/agent/bootstrap/startup_handshake.h"
#include "labbridge/core/version.h"
#include "support/agent/mock_http_server.h"

#include <gtest/gtest.h>

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <type_traits>

namespace {

namespace http = boost::beast::http;
using Json = nlohmann::json;
using labbridge::test::support::MockHttpServer;
using labbridge::test::support::local_server_url;
using namespace std::chrono_literals;

static_assert(std::is_base_of_v<labbridge::agent::IRuntimeControlClient,
                                labbridge::agent::ControlPlaneClient>);

std::string heartbeat_response(std::string_view request_body) {
    const auto request = Json::parse(request_body);
    return Json{
        {"ok", true},
        {"data",
         {
             {"node_code", request.at("node_code")},
             {"status", "online"},
             {"reported_at", request.at("reported_at")},
         }},
    }.dump();
}

TEST(StartupHandshakeTest, RegistersHeartbeatsAndFetchesConfigInOrder) {
    MockHttpServer server{{
        {
            http::status::created,
            R"({"ok":true,"data":{"node_code":"phase20-node","status":"offline"}})",
        },
        {
            http::status::ok,
            {},
            false,
            heartbeat_response,
        },
        {
            http::status::ok,
            R"({
                "ok": true,
                "data": {
                    "node": {
                        "node_code": "phase20-node",
                        "name": "phase20 agent",
                        "agent_version": "0.1.0",
                        "status": "online",
                        "last_heartbeat_at": "2026-07-18T10:00:00Z"
                    },
                    "data_sources": [],
                    "qc_rules": [],
                    "tasks": []
                }
            })",
        },
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    const auto fixed_reported_at = std::chrono::system_clock::from_time_t(0);
    const auto result = labbridge::agent::perform_startup_handshake(
        client,
        {
            "phase20-node",
            "phase20 agent",
            labbridge::core::kVersion,
        },
        fixed_reported_at);
    ASSERT_NO_THROW(server.join());

    EXPECT_EQ(result.node.node_code, "phase20-node");
    EXPECT_EQ(result.status, labbridge::core::NodeStatus::Online);
    EXPECT_TRUE(result.tasks.empty());

    ASSERT_EQ(server.requests().size(), 3U);
    const auto& registration = server.requests()[0];
    EXPECT_EQ(registration.method, http::verb::post);
    EXPECT_EQ(registration.target, "/api/v1/agents/register");
    EXPECT_EQ(registration.content_type, "application/json");
    const auto registration_body = Json::parse(registration.body);
    EXPECT_EQ(registration_body["node_code"], "phase20-node");
    EXPECT_EQ(registration_body["name"], "phase20 agent");
    EXPECT_EQ(registration_body["agent_version"], labbridge::core::kVersion);

    const auto& heartbeat = server.requests()[1];
    EXPECT_EQ(heartbeat.method, http::verb::post);
    EXPECT_EQ(heartbeat.target, "/api/v1/agents/heartbeat");
    EXPECT_EQ(heartbeat.content_type, "application/json");
    const auto heartbeat_body = Json::parse(heartbeat.body);
    EXPECT_EQ(heartbeat_body["node_code"], "phase20-node");
    EXPECT_EQ(heartbeat_body["agent_version"], labbridge::core::kVersion);
    EXPECT_EQ(heartbeat_body["reported_at"].get<std::string>(),
              "1970-01-01T00:00:00Z");

    const auto& config = server.requests()[2];
    EXPECT_EQ(config.method, http::verb::get);
    EXPECT_EQ(config.target, "/api/v1/agents/phase20-node/config");
    EXPECT_TRUE(config.body.empty());
}

}  // namespace
