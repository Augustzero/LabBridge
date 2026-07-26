#include "labbridge/agent/agent_config.h"
#include "labbridge/agent/control_plane_client.h"
#include "labbridge/agent/startup_handshake.h"
#include "labbridge/core/version.h"

#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
using namespace std::chrono_literals;

struct PlannedResponse {
    http::verb method;
    std::string target;
    http::status status;
    std::string body;
    std::chrono::milliseconds delay{0};
};

struct CapturedRequest {
    http::verb method;
    std::string target;
    std::string content_type;
    std::string body;
};

class MockHttpServer final {
public:
    explicit MockHttpServer(std::vector<PlannedResponse> responses)
        : acceptor_(context_, {tcp::v4(), 0}),
          responses_(std::move(responses)),
          thread_([this] { serve(); }) {}

    ~MockHttpServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    unsigned short port() const {
        return acceptor_.local_endpoint().port();
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    const std::vector<CapturedRequest>& requests() const {
        return requests_;
    }

private:
    void serve() {
        try {
            for (const auto& planned : responses_) {
                tcp::socket socket{context_};
                acceptor_.accept(socket);

                beast::flat_buffer buffer;
                http::request<http::string_body> request;
                http::read(socket, buffer, request);
                assert(request.method() == planned.method);
                assert(request.target() == planned.target);
                requests_.push_back({
                    request.method(),
                    std::string{request.target()},
                    std::string{request[http::field::content_type]},
                    request.body(),
                });

                if (planned.delay.count() > 0) {
                    std::this_thread::sleep_for(planned.delay);
                }

                http::response<http::string_body> response{
                    planned.status,
                    request.version()};
                response.set(http::field::content_type, "application/json");
                response.keep_alive(false);
                response.body() = planned.body;
                response.prepare_payload();

                boost::system::error_code ignored;
                http::write(socket, response, ignored);
                socket.shutdown(tcp::socket::shutdown_both, ignored);
            }
        } catch (...) {
            failure_ = std::current_exception();
        }
    }

    asio::io_context context_;
    tcp::acceptor acceptor_;
    std::vector<PlannedResponse> responses_;
    std::vector<CapturedRequest> requests_;
    std::exception_ptr failure_;
    std::thread thread_;
};

std::string server_url(unsigned short port) {
    return "http://127.0.0.1:" + std::to_string(port);
}

void expect_config_error(const std::string& path,
                         const std::string& message_part) {
    try {
        static_cast<void>(labbridge::agent::load_agent_config(path));
        assert(false);
    } catch (const labbridge::agent::AgentConfigError& error) {
        assert(std::string{error.what()}.find(message_part) != std::string::npos);
    }
}

void expect_client_error(
    labbridge::agent::ControlPlaneErrorKind expected_kind,
    const std::function<void()>& action,
    unsigned int expected_status = 0,
    const std::string& expected_server_code = {}) {
    try {
        action();
        assert(false);
    } catch (const labbridge::agent::ControlPlaneClientError& error) {
        assert(error.kind() == expected_kind);
        assert(error.http_status() == expected_status);
        assert(error.server_code() == expected_server_code);
        assert(!std::string{error.what()}.empty());
    }
}

void verify_configuration_loading() {
    const auto config = labbridge::agent::load_agent_config(
        "tests/fixtures/phase20_agent_valid.yaml");
    assert(config.node.node_code == "phase20-node");
    assert(config.node.name == "phase20 agent");
    assert(config.node.agent_version == labbridge::core::kVersion);
    assert(config.server_url == "http://127.0.0.1:18080/");
    assert(config.request_timeout == 7s);

    expect_config_error(
        "tests/fixtures/phase20_agent_missing_name.yaml",
        "agent.name");
    expect_config_error(
        "tests/fixtures/phase20_agent_invalid_timeout.yaml",
        "between 1 and 300");

    try {
        labbridge::agent::validate_control_plane_url(
            "https://127.0.0.1:18080");
        assert(false);
    } catch (const std::invalid_argument& error) {
        assert(std::string{error.what()}.find("http://") != std::string::npos);
    }
}

void verify_startup_handshake_and_empty_tasks() {
    MockHttpServer server{{
        {
            http::verb::post,
            "/api/v1/agents/register",
            http::status::created,
            R"({"ok":true,"data":{"node_code":"phase20-node","status":"offline"}})",
        },
        {
            http::verb::post,
            "/api/v1/agents/heartbeat",
            http::status::ok,
            R"({"ok":true,"data":{"node_code":"phase20-node","status":"online","reported_at":"accepted"}})",
        },
        {
            http::verb::get,
            "/api/v1/agents/phase20-node/config",
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
                    "tasks": []
                }
            })",
        },
    }};

    labbridge::agent::ControlPlaneClient client{
        server_url(server.port()),
        2s};
    const auto result = labbridge::agent::perform_startup_handshake(
        client,
        {
            "phase20-node",
            "phase20 agent",
            labbridge::core::kVersion,
        });
    server.join();

    assert(result.node.node_code == "phase20-node");
    assert(result.status == labbridge::core::NodeStatus::Online);
    assert(result.tasks.empty());
    assert(server.requests().size() == 3);

    const auto& registration = server.requests()[0];
    assert(registration.method == http::verb::post);
    assert(registration.target == "/api/v1/agents/register");
    assert(registration.content_type == "application/json");
    const auto registration_json = Json::parse(registration.body);
    assert(registration_json["node_code"] == "phase20-node");
    assert(registration_json["name"] == "phase20 agent");
    assert(registration_json["agent_version"] == labbridge::core::kVersion);

    const auto& heartbeat = server.requests()[1];
    assert(heartbeat.method == http::verb::post);
    assert(heartbeat.target == "/api/v1/agents/heartbeat");
    const auto heartbeat_json = Json::parse(heartbeat.body);
    assert(heartbeat_json["node_code"] == "phase20-node");
    assert(heartbeat_json["agent_version"] == labbridge::core::kVersion);
    const auto reported_at = heartbeat_json["reported_at"].get<std::string>();
    assert(!reported_at.empty());
    assert(reported_at.back() == 'Z');

    const auto& config = server.requests()[2];
    assert(config.method == http::verb::get);
    assert(config.target == "/api/v1/agents/phase20-node/config");
    assert(config.body.empty());
}

void verify_task_contract() {
    MockHttpServer server{{
        {
            http::verb::get,
            "/api/v1/agents/phase20-node/config",
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
                    "tasks": [{
                        "id": "task-20",
                        "node_code": "phase20-node",
                        "data_source_id": "source-20",
                        "name": "phase20 task",
                        "task_type": "collect_parse_qc",
                        "schedule_expr": "*/5 * * * *",
                        "parser_type": "csv_observation",
                        "qc_profile": "basic",
                        "enabled": true
                    }]
                }
            })",
        },
    }};

    labbridge::agent::ControlPlaneClient client{
        server_url(server.port()),
        2s};
    const auto result = client.fetch_config("phase20-node");
    server.join();

    assert(result.tasks.size() == 1);
    const auto& task = result.tasks.front();
    assert(task.id == "task-20");
    assert(task.data_source_id == "source-20");
    assert(task.task_type == "collect_parse_qc");
    assert(task.qc_profile == "basic");
    assert(task.enabled);
}

void verify_response_error_classification() {
    {
        MockHttpServer server{{
            {
                http::verb::post,
                "/api/v1/agents/register",
                http::status::ok,
                "{",
            },
        }};
        labbridge::agent::ControlPlaneClient client{
            server_url(server.port()),
            2s};
        expect_client_error(
            labbridge::agent::ControlPlaneErrorKind::InvalidJson,
            [&] {
                client.register_node(
                    {"phase20-node", "phase20 agent", "0.1.0"});
            },
            200);
        server.join();
    }

    {
        MockHttpServer server{{
            {
                http::verb::post,
                "/api/v1/agents/register",
                http::status::not_found,
                R"({"ok":false,"error":{"code":"not_found","message":"node not found"}})",
            },
        }};
        labbridge::agent::ControlPlaneClient client{
            server_url(server.port()),
            2s};
        expect_client_error(
            labbridge::agent::ControlPlaneErrorKind::ServerError,
            [&] {
                client.register_node(
                    {"phase20-node", "phase20 agent", "0.1.0"});
            },
            404,
            "not_found");
        server.join();
    }

    {
        MockHttpServer server{{
            {
                http::verb::post,
                "/api/v1/agents/register",
                http::status::service_unavailable,
                "temporarily unavailable",
            },
        }};
        labbridge::agent::ControlPlaneClient client{
            server_url(server.port()),
            2s};
        expect_client_error(
            labbridge::agent::ControlPlaneErrorKind::HttpStatus,
            [&] {
                client.register_node(
                    {"phase20-node", "phase20 agent", "0.1.0"});
            },
            503);
        server.join();
    }
}

void verify_network_failure_and_timeout() {
    asio::io_context context;
    tcp::acceptor unused_port{context, {tcp::v4(), 0}};
    const auto port = unused_port.local_endpoint().port();
    unused_port.close();

    labbridge::agent::ControlPlaneClient unavailable_client{
        server_url(port),
        500ms};
    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::Network,
        [&] {
            unavailable_client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        });

    MockHttpServer slow_server{{
        {
            http::verb::post,
            "/api/v1/agents/register",
            http::status::created,
            R"({"ok":true,"data":{"node_code":"phase20-node","status":"offline"}})",
            250ms,
        },
    }};
    labbridge::agent::ControlPlaneClient slow_client{
        server_url(slow_server.port()),
        50ms};
    expect_client_error(
        labbridge::agent::ControlPlaneErrorKind::Network,
        [&] {
            slow_client.register_node(
                {"phase20-node", "phase20 agent", "0.1.0"});
        });
    slow_server.join();
}

}  // namespace

int main() {
    verify_configuration_loading();
    verify_startup_handshake_and_empty_tasks();
    verify_task_contract();
    verify_response_error_classification();
    verify_network_failure_and_timeout();
    return 0;
}
