#include "labbridge/server/http/task_run_http_controller.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace labbridge::server {
namespace {

constexpr std::string_view kComponent = "task-run-http";

std::string required_string(const Json::Value& body, const std::string& field) {
    if (!body.isMember(field)) {
        throw http::RequestValidationError(field + " is required");
    }
    if (!body[field].isString()) {
        throw http::RequestValidationError(field + " must be a string");
    }
    const auto value = body[field].asString();
    if (value.empty()) {
        throw http::RequestValidationError(field + " must not be empty");
    }
    return value;
}

StartTaskRunRequest parse_start_request(const drogon::HttpRequestPtr& request) {
    const auto& body = request->getJsonObject();
    if (!body || !body->isObject()) {
        throw http::RequestValidationError(
            "request body must contain a JSON object");
    }

    StartTaskRunRequest parsed;
    parsed.node_code = required_string(*body, "node_code");
    parsed.task_id = required_string(*body, "task_id");
    parsed.execution_key = required_string(*body, "execution_key");
    parsed.scheduled_for = required_string(*body, "scheduled_for");
    parsed.started_at = required_string(*body, "started_at");
    parsed.trigger_type = required_string(*body, "trigger_type");
    return parsed;
}

}  // namespace

TaskRunHttpController::TaskRunHttpController(StartHandler start_handler)
    : start_handler_(std::move(start_handler)) {
    if (!start_handler_) {
        throw std::invalid_argument("task run start HTTP handler is required");
    }
}

void TaskRunHttpController::register_routes(drogon::HttpAppFramework& app) {
    auto self = shared_from_this();
    app.registerHandler(
        "/api/v1/task-runs/start",
        [self](const drogon::HttpRequestPtr& request, ResponseCallback&& callback) {
            self->post_start(request, std::move(callback));
        },
        {drogon::Post});
}

void TaskRunHttpController::post_start(
    const drogon::HttpRequestPtr& request,
    ResponseCallback&& callback) const {
    http::handle_request(kComponent, "POST /api/v1/task-runs/start", [&] {
        if (!http::require_json_content_type(request, callback)) {
            return;
        }
        const auto result = start_handler_(parse_start_request(request));
        if (!result.status.ok) {
            callback(http::status_error_response(result.status));
            return;
        }

        Json::Value data;
        data["task_run_id"] = result.id;
        data["replayed"] = result.replayed;
        callback(http::success_response(drogon::k201Created, std::move(data)));
    }, callback);
}

}  // namespace labbridge::server
