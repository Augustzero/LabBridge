#include "labbridge/server/agent_report_http_controller.h"

#include "labbridge/core/logging.h"

#include <json/reader.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace labbridge::server {
namespace {

constexpr std::string_view kComponent = "agent-report-http";

class RequestValidationError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string field_path(const std::string& object_path, const std::string& field) {
    return object_path.empty() ? field : object_path + "." + field;
}

std::string required_string(const Json::Value& object,
                            const std::string& field,
                            const std::string& object_path) {
    const auto path = field_path(object_path, field);
    if (!object.isMember(field)) {
        throw RequestValidationError(path + " is required");
    }
    if (!object[field].isString()) {
        throw RequestValidationError(path + " must be a string");
    }
    return object[field].asString();
}

std::string optional_string(const Json::Value& object,
                            const std::string& field,
                            const std::string& default_value,
                            const std::string& object_path) {
    if (!object.isMember(field)) {
        return default_value;
    }
    if (!object[field].isString()) {
        throw RequestValidationError(field_path(object_path, field) + " must be a string");
    }
    return object[field].asString();
}

int optional_int(const Json::Value& object,
                 const std::string& field,
                 int default_value,
                 const std::string& object_path) {
    if (!object.isMember(field)) {
        return default_value;
    }
    if (!object[field].isInt()) {
        throw RequestValidationError(field_path(object_path, field) + " must be an integer");
    }
    return object[field].asInt();
}

long long optional_int64(const Json::Value& object,
                         const std::string& field,
                         long long default_value,
                         const std::string& object_path) {
    if (!object.isMember(field)) {
        return default_value;
    }
    if (!object[field].isInt64()) {
        throw RequestValidationError(field_path(object_path, field) + " must be an integer");
    }
    return object[field].asInt64();
}

const Json::Value& optional_array(const Json::Value& object,
                                  const std::string& field,
                                  const std::string& object_path) {
    static const Json::Value empty_array{Json::arrayValue};
    if (!object.isMember(field)) {
        return empty_array;
    }
    if (!object[field].isArray()) {
        throw RequestValidationError(field_path(object_path, field) + " must be an array");
    }
    return object[field];
}

void require_object(const Json::Value& value, const std::string& path) {
    if (!value.isObject()) {
        throw RequestValidationError(path + " must be an object");
    }
}

bool is_valid_json_text(const std::string& text) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    auto reader = std::unique_ptr<Json::CharReader>{builder.newCharReader()};
    Json::Value parsed;
    std::string errors;
    return reader->parse(text.data(), text.data() + text.size(), &parsed, &errors);
}

labbridge::core::TaskRunStatus parse_task_run_status(const std::string& status) {
    if (status == "succeeded") {
        return labbridge::core::TaskRunStatus::Succeeded;
    }
    if (status == "failed") {
        return labbridge::core::TaskRunStatus::Failed;
    }
    throw RequestValidationError("status must be succeeded or failed");
}

RawFileManifestRequest parse_raw_file_manifest(const Json::Value& body) {
    require_object(body, "request body");

    RawFileManifestRequest request;
    request.task_run_id = required_string(body, "task_run_id", "");
    request.node_code = required_string(body, "node_code", "");

    const auto& files = optional_array(body, "files", "");
    request.files.reserve(files.size());
    for (Json::ArrayIndex index = 0; index < files.size(); ++index) {
        const auto path = "files[" + std::to_string(index) + "]";
        const auto& value = files[index];
        require_object(value, path);

        RawFileManifestEntry entry;
        entry.original_name = required_string(value, "original_name", path);
        entry.file_hash = optional_string(value, "file_hash", {}, path);
        entry.storage_path = required_string(value, "storage_path", path);
        entry.size_bytes = optional_int64(value, "size_bytes", 0, path);
        entry.source_mtime = optional_string(value, "source_mtime", {}, path);
        entry.ingest_status =
            optional_string(value, "ingest_status", entry.ingest_status, path);
        request.files.push_back(std::move(entry));
    }

    return request;
}

TaskRunReportRequest parse_task_run_report(const Json::Value& body) {
    require_object(body, "request body");

    TaskRunReportRequest request;
    request.task_run_id = required_string(body, "task_run_id", "");
    request.node_code = required_string(body, "node_code", "");
    request.status = parse_task_run_status(required_string(body, "status", ""));
    request.finished_at = optional_string(body, "finished_at", {}, "");
    request.items_total = optional_int(body, "items_total", 0, "");
    request.items_success = optional_int(body, "items_success", 0, "");
    request.items_failed = optional_int(body, "items_failed", 0, "");
    request.error_summary = optional_string(body, "error_summary", {}, "");

    const auto& parsed_records = optional_array(body, "parsed_records", "");
    request.parsed_records.reserve(parsed_records.size());
    for (Json::ArrayIndex index = 0; index < parsed_records.size(); ++index) {
        const auto path = "parsed_records[" + std::to_string(index) + "]";
        const auto& value = parsed_records[index];
        require_object(value, path);

        TaskRunReportParsedRecord parsed;
        parsed.raw_file_id = required_string(value, "raw_file_id", path);
        parsed.record.station_code = optional_string(value, "station_code", {}, path);
        parsed.record.device_code = optional_string(value, "device_code", {}, path);
        parsed.record.record_time = optional_string(value, "record_time", {}, path);
        parsed.record.payload_json = required_string(value, "payload_json", path);
        if (!is_valid_json_text(parsed.record.payload_json)) {
            throw RequestValidationError(
                field_path(path, "payload_json") + " must contain valid JSON");
        }
        parsed.parse_status =
            optional_string(value, "parse_status", parsed.parse_status, path);

        const auto& qc_results = optional_array(value, "qc_results", path);
        parsed.qc_results.reserve(qc_results.size());
        for (Json::ArrayIndex qc_index = 0; qc_index < qc_results.size(); ++qc_index) {
            const auto qc_path =
                field_path(path, "qc_results") + "[" + std::to_string(qc_index) + "]";
            const auto& qc_value = qc_results[qc_index];
            require_object(qc_value, qc_path);

            TaskRunReportQcResult qc;
            qc.qc_rule_id = required_string(qc_value, "qc_rule_id", qc_path);
            qc.level = required_string(qc_value, "level", qc_path);
            qc.result = required_string(qc_value, "result", qc_path);
            qc.message = optional_string(qc_value, "message", {}, qc_path);
            parsed.qc_results.push_back(std::move(qc));
        }

        request.parsed_records.push_back(std::move(parsed));
    }

    return request;
}

Json::Value string_array(const std::vector<std::string>& values) {
    Json::Value array{Json::arrayValue};
    for (const auto& value : values) {
        array.append(value);
    }
    return array;
}

drogon::HttpResponsePtr json_response(drogon::HttpStatusCode status, Json::Value body) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    response->setStatusCode(status);
    return response;
}

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

bool validate_content_type(const drogon::HttpRequestPtr& request,
                           AgentReportHttpController::ResponseCallback& callback) {
    if (request->contentType() == drogon::CT_APPLICATION_JSON) {
        return true;
    }
    callback(error_response(drogon::k415UnsupportedMediaType,
                            "unsupported_media_type",
                            "content type must be application/json"));
    return false;
}

const Json::Value& parse_json_body(const drogon::HttpRequestPtr& request) {
    const auto& body = request->getJsonObject();
    if (!body) {
        throw RequestValidationError("request body must contain valid JSON");
    }
    return *body;
}

void handle_unexpected_exception(const std::exception& error,
                                 AgentReportHttpController::ResponseCallback& callback) {
    labbridge::core::log_error(kComponent, error.what());
    callback(error_response(
        drogon::k500InternalServerError, "internal_error", "internal server error"));
}

void handle_unknown_exception(AgentReportHttpController::ResponseCallback& callback) {
    labbridge::core::log_error(kComponent, "unknown exception");
    callback(error_response(
        drogon::k500InternalServerError, "internal_error", "internal server error"));
}

}  // namespace

AgentReportHttpController::AgentReportHttpController(
    RawFileManifestHandler raw_file_manifest_handler,
    TaskRunReportHandler task_run_report_handler)
    : raw_file_manifest_handler_(std::move(raw_file_manifest_handler)),
      task_run_report_handler_(std::move(task_run_report_handler)) {
    if (!raw_file_manifest_handler_ || !task_run_report_handler_) {
        throw std::invalid_argument("agent report HTTP handlers are required");
    }
}

void AgentReportHttpController::register_routes(drogon::HttpAppFramework& app) {
    auto self = shared_from_this();
    app.registerHandler(
        "/api/v1/raw-files/manifest",
        [self](const drogon::HttpRequestPtr& request, ResponseCallback&& callback) {
            self->post_raw_file_manifest(request, std::move(callback));
        },
        {drogon::Post});
    app.registerHandler(
        "/api/v1/task-runs/report",
        [self](const drogon::HttpRequestPtr& request, ResponseCallback&& callback) {
            self->post_task_run_report(request, std::move(callback));
        },
        {drogon::Post});
}

void AgentReportHttpController::post_raw_file_manifest(
    const drogon::HttpRequestPtr& request,
    ResponseCallback&& callback) const {
    if (!validate_content_type(request, callback)) {
        return;
    }

    try {
        const auto parsed = parse_raw_file_manifest(parse_json_body(request));
        const auto result = raw_file_manifest_handler_(parsed);
        if (!result.status.ok) {
            callback(status_error_response(result.status));
            return;
        }

        Json::Value data;
        data["raw_file_ids"] = string_array(result.raw_file_ids);
        callback(success_response(drogon::k201Created, std::move(data)));
    } catch (const RequestValidationError& error) {
        callback(error_response(drogon::k400BadRequest, "invalid_argument", error.what()));
    } catch (const std::exception& error) {
        handle_unexpected_exception(error, callback);
    } catch (...) {
        handle_unknown_exception(callback);
    }
}

void AgentReportHttpController::post_task_run_report(
    const drogon::HttpRequestPtr& request,
    ResponseCallback&& callback) const {
    if (!validate_content_type(request, callback)) {
        return;
    }

    try {
        const auto parsed = parse_task_run_report(parse_json_body(request));
        const auto result = task_run_report_handler_(parsed);
        if (!result.status.ok) {
            callback(status_error_response(result.status));
            return;
        }

        Json::Value data;
        data["parsed_record_ids"] = string_array(result.parsed_record_ids);
        data["qc_result_ids"] = string_array(result.qc_result_ids);
        data["alert_ids"] = string_array(result.alert_ids);
        callback(success_response(drogon::k200OK, std::move(data)));
    } catch (const RequestValidationError& error) {
        callback(error_response(drogon::k400BadRequest, "invalid_argument", error.what()));
    } catch (const std::exception& error) {
        handle_unexpected_exception(error, callback);
    } catch (...) {
        handle_unknown_exception(callback);
    }
}

}  // namespace labbridge::server
