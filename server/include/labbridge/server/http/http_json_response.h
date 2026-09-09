#pragma once

#include "labbridge/core/logging.h"
#include "labbridge/core/result.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace labbridge::server::http {

using ResponseCallback = std::function<void(const drogon::HttpResponsePtr&)>;

// 控制器共享的请求校验失败类型：统一映射为 400 invalid_argument。
class RequestValidationError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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

// 控制器统一的请求处理封装：校验失败 -> 400，其余异常 -> 500，
// 并输出 route status duration_ms 访问日志。
template <typename Operation>
void handle_request(std::string_view component,
                    std::string_view route,
                    Operation operation,
                    ResponseCallback& callback) {
    const auto started_at = std::chrono::steady_clock::now();
    drogon::HttpResponsePtr emitted_response;
    auto original_callback = std::move(callback);
    callback = [&emitted_response, &original_callback](
                   const drogon::HttpResponsePtr& response) {
        emitted_response = response;
        original_callback(response);
    };

    try {
        operation();
    } catch (const RequestValidationError& error) {
        callback(error_response(
            drogon::k400BadRequest, "invalid_argument", error.what()));
    } catch (const std::exception& error) {
        handle_unexpected_exception(component, error, callback);
    } catch (...) {
        handle_unknown_exception(component, callback);
    }

    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    const int status = emitted_response == nullptr
        ? 0
        : static_cast<int>(emitted_response->statusCode());
    labbridge::core::log_info(
        component,
        std::string{route} +
            " status=" + std::to_string(status) +
            " duration_ms=" + std::to_string(duration.count()));
}

}  // namespace labbridge::server::http
