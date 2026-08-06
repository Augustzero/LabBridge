#include "labbridge/server/http/http_json_response.h"

#include "labbridge/core/logging.h"

#include <utility>

namespace labbridge::server::http {
namespace {

drogon::HttpResponsePtr json_response(drogon::HttpStatusCode status,
                                      Json::Value body) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    response->setStatusCode(status);
    return response;
}

}  // namespace

drogon::HttpResponsePtr success_response(drogon::HttpStatusCode status,
                                         Json::Value data) {
    Json::Value body;
    body["ok"] = true;
    body["data"] = std::move(data);
    return json_response(status, std::move(body));
}

drogon::HttpResponsePtr error_response(drogon::HttpStatusCode status,
                                       const std::string& code,
                                       const std::string& message) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = code;
    body["error"]["message"] = message;
    return json_response(status, std::move(body));
}

drogon::HttpResponsePtr status_error_response(const labbridge::core::Status& status) {
    switch (status.code) {
        case labbridge::core::StatusCode::InvalidArgument:
            return error_response(
                drogon::k400BadRequest, "invalid_argument", status.message);
        case labbridge::core::StatusCode::NotFound:
            return error_response(drogon::k404NotFound, "not_found", status.message);
        case labbridge::core::StatusCode::Conflict:
            return error_response(drogon::k409Conflict, "conflict", status.message);
        case labbridge::core::StatusCode::Ok:
            break;
    }
    return error_response(
        drogon::k500InternalServerError, "internal_error", "internal server error");
}

bool require_json_content_type(const drogon::HttpRequestPtr& request,
                               ResponseCallback& callback) {
    if (request->contentType() == drogon::CT_APPLICATION_JSON) {
        return true;
    }
    callback(error_response(drogon::k415UnsupportedMediaType,
                            "unsupported_media_type",
                            "content type must be application/json"));
    return false;
}

void handle_unexpected_exception(std::string_view component,
                                 const std::exception& error,
                                 ResponseCallback& callback) {
    labbridge::core::log_error(component, error.what());
    callback(error_response(
        drogon::k500InternalServerError, "internal_error", "internal server error"));
}

void handle_unknown_exception(std::string_view component,
                              ResponseCallback& callback) {
    labbridge::core::log_error(component, "unknown exception");
    callback(error_response(
        drogon::k500InternalServerError, "internal_error", "internal server error"));
}

}  // namespace labbridge::server::http
