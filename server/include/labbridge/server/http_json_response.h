#pragma once

#include "labbridge/core/result.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <exception>
#include <functional>
#include <string>
#include <string_view>

namespace labbridge::server::http {

using ResponseCallback = std::function<void(const drogon::HttpResponsePtr&)>;

drogon::HttpResponsePtr success_response(drogon::HttpStatusCode status,
                                         Json::Value data);
drogon::HttpResponsePtr error_response(drogon::HttpStatusCode status,
                                       const std::string& code,
                                       const std::string& message);
drogon::HttpResponsePtr status_error_response(const labbridge::core::Status& status);

bool require_json_content_type(const drogon::HttpRequestPtr& request,
                               ResponseCallback& callback);
void handle_unexpected_exception(std::string_view component,
                                 const std::exception& error,
                                 ResponseCallback& callback);
void handle_unknown_exception(std::string_view component,
                              ResponseCallback& callback);

}  // namespace labbridge::server::http
