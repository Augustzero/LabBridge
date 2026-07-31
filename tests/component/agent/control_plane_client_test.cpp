#include "labbridge/agent/control_plane_client.h"
#include "support/agent/mock_http_server.h"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
using labbridge::test::support::MockHttpServer;
using labbridge::test::support::local_server_url;
using namespace std::chrono_literals;

std::string config_response(const std::string& node_code,
                            Json tasks = Json::array()) {
    Json data;
    data["node"] = {
        {"node_code", node_code},
        {"name", "phase20 agent"},
        {"agent_version", "0.1.0"},
        {"status", "online"},
        {"last_heartbeat_at", "2026-07-18T10:00:00Z"},
    };
    data["tasks"] = std::move(tasks);
    return Json{{"ok", true}, {"data", std::move(data)}}.dump();
}

template <typename Action>
void expect_client_error(
    labbridge::agent::ControlPlaneErrorKind expected_kind,
    Action action,
    unsigned int expected_status = 0,
    const std::string& expected_server_code = {}) {
    try {
        action();
        FAIL() << "expected ControlPlaneClientError";
    } catch (const labbridge::agent::ControlPlaneClientError& error) {
        EXPECT_EQ(error.kind(), expected_kind);
        EXPECT_EQ(error.http_status(), expected_status);
        EXPECT_EQ(error.server_code(), expected_server_code);
        EXPECT_FALSE(std::string{error.what()}.empty());
    } catch (const std::exception& error) {
        FAIL() << "unexpected exception: " << error.what();
    }
}

TEST(ControlPlaneClientTest, RejectsUnsupportedServerScheme) {
    EXPECT_THROW(
        labbridge::agent::validate_control_plane_url(
            "https://127.0.0.1:18080"),
        std::invalid_argument);
}

TEST(ControlPlaneClientTest, SendsRegistrationContract) {
    MockHttpServer server{{
        {
            http::status::created,
            R"({"ok":true,"data":{"node_code":"phase20-node","status":"offline"}})",
        },
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    client.register_node(
        {"phase20-node", "phase20 agent", "0.1.0"});
    ASSERT_NO_THROW(server.join());

    ASSERT_EQ(server.requests().size(), 1U);
    const auto& request = server.requests().front();
    EXPECT_EQ(request.method, http::verb::post);
    EXPECT_EQ(request.target, "/api/v1/agents/register");
    EXPECT_EQ(request.content_type, "application/json");
    const auto body = Json::parse(request.body);
    EXPECT_EQ(body["node_code"], "phase20-node");
    EXPECT_EQ(body["name"], "phase20 agent");
    EXPECT_EQ(body["agent_version"], "0.1.0");
}

TEST(ControlPlaneClientTest, SendsHeartbeatContract) {
    MockHttpServer server{{
        {
            http::status::ok,
            R"({"ok":true,"data":{"node_code":"phase20-node","status":"online"}})",
        },
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    client.send_heartbeat({
        "phase20-node",
        "0.1.0",
        "2026-07-18T10:00:00Z",
    });
    ASSERT_NO_THROW(server.join());

    ASSERT_EQ(server.requests().size(), 1U);
    const auto& request = server.requests().front();
    EXPECT_EQ(request.method, http::verb::post);
    EXPECT_EQ(request.target, "/api/v1/agents/heartbeat");
    EXPECT_EQ(request.content_type, "application/json");
    const auto body = Json::parse(request.body);
    EXPECT_EQ(body["node_code"], "phase20-node");
    EXPECT_EQ(body["agent_version"], "0.1.0");
    EXPECT_EQ(body["reported_at"], "2026-07-18T10:00:00Z");
}

TEST(ControlPlaneClientTest, EncodesConfigPathAndMapsEnabledTasks) {
    const std::string node_code = "phase20 node/a";
    Json tasks = Json::array({
        {
            {"id", "task-20"},
            {"node_code", node_code},
            {"data_source_id", "source-20"},
            {"name", "phase20 task"},
            {"task_type", "collect_parse_qc"},
            {"schedule_expr", "*/5 * * * *"},
            {"parser_type", "csv_observation"},
            {"qc_profile", "basic"},
            {"enabled", true},
        },
    });
    MockHttpServer server{{
        {http::status::ok, config_response(node_code, std::move(tasks))},
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    const auto config = client.fetch_config(node_code);
    ASSERT_NO_THROW(server.join());

    EXPECT_EQ(config.node.node_code, node_code);
    EXPECT_EQ(config.status, labbridge::core::NodeStatus::Online);
    EXPECT_EQ(config.last_heartbeat_at, "2026-07-18T10:00:00Z");
    ASSERT_EQ(config.tasks.size(), 1U);
    const auto& task = config.tasks.front();
    EXPECT_EQ(task.id, "task-20");
    EXPECT_EQ(task.data_source_id, "source-20");
    EXPECT_EQ(task.task_type, "collect_parse_qc");
    EXPECT_EQ(task.qc_profile, "basic");
    EXPECT_TRUE(task.enabled);

    ASSERT_EQ(server.requests().size(), 1U);
    EXPECT_EQ(server.requests().front().method, http::verb::get);
    EXPECT_EQ(server.requests().front().target,
              "/api/v1/agents/phase20%20node%2Fa/config");
    EXPECT_TRUE(server.requests().front().body.empty());
}

TEST(ControlPlaneClientTest, AcceptsEmptyEnabledTaskList) {
    MockHttpServer server{{
        {http::status::ok, config_response("phase20-node")},
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    const auto config = client.fetch_config("phase20-node");
    ASSERT_NO_THROW(server.join());

    EXPECT_TRUE(config.tasks.empty());
}

TEST(ControlPlaneClientTest, ClassifiesInvalidJsonResponse) {
    MockHttpServer server{{
        {http::status::ok, "{"},
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::InvalidJson,
        [&] {
            client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        },
        200);
    ASSERT_NO_THROW(server.join());
}

TEST(ControlPlaneClientTest, PreservesStructuredServerError) {
    MockHttpServer server{{
        {
            http::status::not_found,
            R"({"ok":false,"error":{"code":"not_found","message":"node not found"}})",
        },
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::ServerError,
        [&] {
            client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        },
        404,
        "not_found");
    ASSERT_NO_THROW(server.join());
}

TEST(ControlPlaneClientTest, ClassifiesUnstructuredHttpFailure) {
    MockHttpServer server{{
        {http::status::service_unavailable, "temporarily unavailable"},
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::HttpStatus,
        [&] {
            client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        },
        503);
    ASSERT_NO_THROW(server.join());
}

TEST(ControlPlaneClientTest, RejectsInvalidSuccessEnvelope) {
    MockHttpServer server{{
        {http::status::ok, R"({"ok":true})"},
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::InvalidResponse,
        [&] {
            client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        },
        200);
    ASSERT_NO_THROW(server.join());
}

TEST(ControlPlaneClientTest, ClassifiesConnectionFailure) {
    asio::io_context context;
    tcp::acceptor unused_port{context, {tcp::v4(), 0}};
    const auto port = unused_port.local_endpoint().port();
    unused_port.close();
    labbridge::agent::ControlPlaneClient client{
        local_server_url(port),
        500ms};

    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::Network,
        [&] {
            client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        });
}

TEST(ControlPlaneClientTest, EnforcesOverallRequestTimeout) {
    MockHttpServer server{{
        {
            http::status::created,
            R"({"ok":true,"data":{"node_code":"phase20-node","status":"offline"}})",
            250ms,
        },
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        50ms};

    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::Network,
        [&] {
            client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        });
    ASSERT_NO_THROW(server.join());
}

TEST(ControlPlaneClientTest, RejectsResponseBodyAboveOneMebibyte) {
    MockHttpServer server{{
        {
            http::status::ok,
            std::string((1024U * 1024U) + 1U, 'x'),
        },
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()),
        2s};

    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::Network,
        [&] {
            client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        });
    ASSERT_NO_THROW(server.join());
}

}  // namespace
