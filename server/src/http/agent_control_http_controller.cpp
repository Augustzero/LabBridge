#include "labbridge/server/http/agent_control_http_controller.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace labbridge::server {
namespace {

constexpr std::string_view kComponent = "agent-control-http";

class RequestValidationError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require_object(const Json::Value& value, const std::string& path) {
    if (!value.isObject()) {
        throw RequestValidationError(path + " must be an object");
    }
}

std::string required_string(const Json::Value& object,
                            const std::string& field) {
    if (!object.isMember(field)) {
        throw RequestValidationError(field + " is required");
    }
    if (!object[field].isString()) {
        throw RequestValidationError(field + " must be a string");
    }
    return object[field].asString();
}

const Json::Value& parse_json_body(const drogon::HttpRequestPtr& request) {
    const auto& body = request->getJsonObject();
    if (!body) {
        throw RequestValidationError("request body must contain valid JSON");
    }
    require_object(*body, "request body");
    return *body;
}

labbridge::core::NodeInfo parse_registration(const Json::Value& body) {
    labbridge::core::NodeInfo node;
    node.node_code = required_string(body, "node_code");
    node.name = required_string(body, "name");
    node.agent_version = required_string(body, "agent_version");
    return node;
}

labbridge::core::NodeHeartbeat parse_heartbeat(const Json::Value& body) {
    labbridge::core::NodeHeartbeat heartbeat;
    heartbeat.node_code = required_string(body, "node_code");
    heartbeat.agent_version = required_string(body, "agent_version");
    heartbeat.reported_at = required_string(body, "reported_at");
    return heartbeat;
}

std::string node_status(labbridge::core::NodeStatus status) {
    return status == labbridge::core::NodeStatus::Online ? "online" : "offline";
}

Json::Value task_json(const TaskRecord& task) {
    Json::Value value;
    value["id"] = task.id;
    value["node_code"] = task.node_code;
    value["data_source_id"] = task.data_source_id;
    value["name"] = task.name;
    value["task_type"] = task.task_type;
    value["schedule_expr"] = task.schedule_expr;
    value["parser_type"] = task.parser_type;
    value["qc_profile"] = task.qc_profile;
    value["enabled"] = task.enabled;
    return value;
}

Json::Value config_json(const AgentConfigResult& result) {
    Json::Value data;
    const auto& node = *result.node;
    data["node"]["node_code"] = node.info.node_code;
    data["node"]["name"] = node.info.name;
    data["node"]["agent_version"] = node.info.agent_version;
    data["node"]["status"] = node_status(node.status);
    data["node"]["last_heartbeat_at"] = node.last_heartbeat_at;

    data["tasks"] = Json::Value{Json::arrayValue};
    for (const auto& task : result.enabled_tasks) {
        data["tasks"].append(task_json(task));
    }
    return data;
}

}  // namespace

AgentControlHttpController::AgentControlHttpController(
    RegisterNodeHandler register_node_handler,
    HeartbeatHandler heartbeat_handler,
    FindConfigHandler find_config_handler)
    : register_node_handler_(std::move(register_node_handler)),
      heartbeat_handler_(std::move(heartbeat_handler)),
      find_config_handler_(std::move(find_config_handler)) {
    if (!register_node_handler_ || !heartbeat_handler_ || !find_config_handler_) {
        throw std::invalid_argument("agent control HTTP handlers are required");
    }
}

void AgentControlHttpController::register_routes(drogon::HttpAppFramework& app) {
    auto self = shared_from_this();
    app.registerHandler(
        "/api/v1/agents/register",
        [self](const drogon::HttpRequestPtr& request, ResponseCallback&& callback) {
            self->post_register(request, std::move(callback));
        },
        {drogon::Post});
    app.registerHandler(
        "/api/v1/agents/heartbeat",
        [self](const drogon::HttpRequestPtr& request, ResponseCallback&& callback) {
            self->post_heartbeat(request, std::move(callback));
        },
        {drogon::Post});
    app.registerHandler(
        "/api/v1/agents/{1}/config",
        [self](const drogon::HttpRequestPtr&,
               ResponseCallback&& callback,
               const std::string& node_code) {
            self->get_config(node_code, std::move(callback));
        },
        {drogon::Get});
}

void AgentControlHttpController::post_register(
    const drogon::HttpRequestPtr& request,
    ResponseCallback&& callback) const {
    if (!http::require_json_content_type(request, callback)) {
        return;
    }

    try {
        const auto node = parse_registration(parse_json_body(request));
        const auto status = register_node_handler_(node);
        if (!status.ok) {
            callback(http::status_error_response(status));
            return;
        }

        Json::Value data;
        data["node_code"] = node.node_code;
        data["status"] = "offline";
        callback(http::success_response(drogon::k201Created, std::move(data)));
    } catch (const RequestValidationError& error) {
        callback(http::error_response(
            drogon::k400BadRequest, "invalid_argument", error.what()));
    } catch (const std::exception& error) {
        http::handle_unexpected_exception(kComponent, error, callback);
    } catch (...) {
        http::handle_unknown_exception(kComponent, callback);
    }
}

void AgentControlHttpController::post_heartbeat(
    const drogon::HttpRequestPtr& request,
    ResponseCallback&& callback) const {
    if (!http::require_json_content_type(request, callback)) {
        return;
    }

    try {
        const auto heartbeat = parse_heartbeat(parse_json_body(request));
        const auto status = heartbeat_handler_(heartbeat);
        if (!status.ok) {
            callback(http::status_error_response(status));
            return;
        }

        Json::Value data;
        data["node_code"] = heartbeat.node_code;
        data["status"] = "online";
        data["reported_at"] = heartbeat.reported_at;
        callback(http::success_response(drogon::k200OK, std::move(data)));
    } catch (const RequestValidationError& error) {
        callback(http::error_response(
            drogon::k400BadRequest, "invalid_argument", error.what()));
    } catch (const std::exception& error) {
        http::handle_unexpected_exception(kComponent, error, callback);
    } catch (...) {
        http::handle_unknown_exception(kComponent, callback);
    }
}

void AgentControlHttpController::get_config(
    const std::string& node_code,
    ResponseCallback&& callback) const {
    try {
        const auto result = find_config_handler_(node_code);
        if (!result.status.ok) {
            callback(http::status_error_response(result.status));
            return;
        }
        if (!result.node.has_value()) {
            throw std::runtime_error("agent config result is missing node");
        }
        callback(http::success_response(
            drogon::k200OK, config_json(result)));
    } catch (const std::exception& error) {
        http::handle_unexpected_exception(kComponent, error, callback);
    } catch (...) {
        http::handle_unknown_exception(kComponent, callback);
    }
}

}  // namespace labbridge::server
