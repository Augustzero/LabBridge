#include "labbridge/server/http/management_http_controller.h"
#include "labbridge/core/logging.h"

#include <json/writer.h>

#include <charconv>
#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace labbridge::server {
namespace {

constexpr std::string_view kComponent = "management-http";

class RequestValidationError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Parameters = std::unordered_map<std::string, std::string,
                                      drogon::utils::internal::SafeStringHash>;

void require_allowed_parameters(
    const Parameters& parameters,
    std::initializer_list<std::string_view> allowed) {
    std::set<std::string, std::less<>> names;
    for (const auto name : allowed) {
        names.emplace(name);
    }
    for (const auto& parameter : parameters) {
        if (names.find(parameter.first) == names.end()) {
            throw RequestValidationError(
                "unknown query parameter: " + parameter.first);
        }
    }
}

void require_allowed_members(
    const Json::Value& object,
    std::initializer_list<std::string_view> allowed) {
    std::set<std::string, std::less<>> names;
    for (const auto name : allowed) {
        names.emplace(name);
    }
    for (const auto& member : object.getMemberNames()) {
        if (names.find(member) == names.end()) {
            throw RequestValidationError("unknown body field: " + member);
        }
    }
}

const Json::Value& parse_json_body(const drogon::HttpRequestPtr& request) {
    const auto& body = request->getJsonObject();
    if (body == nullptr || !body->isObject()) {
        throw RequestValidationError(
            "request body must contain a JSON object");
    }
    return *body;
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

bool required_bool(const Json::Value& object, const std::string& field) {
    if (!object.isMember(field)) {
        throw RequestValidationError(field + " is required");
    }
    if (!object[field].isBool()) {
        throw RequestValidationError(field + " must be a boolean");
    }
    return object[field].asBool();
}

const Json::Value& required_object(const Json::Value& object,
                                   const std::string& field) {
    if (!object.isMember(field)) {
        throw RequestValidationError(field + " is required");
    }
    if (!object[field].isObject()) {
        throw RequestValidationError(field + " must be an object");
    }
    return object[field];
}

std::vector<std::string> required_string_array(
    const Json::Value& object,
    const std::string& field) {
    if (!object.isMember(field)) {
        throw RequestValidationError(field + " is required");
    }
    if (!object[field].isArray()) {
        throw RequestValidationError(field + " must be an array");
    }
    std::vector<std::string> values;
    for (const auto& value : object[field]) {
        if (!value.isString()) {
            throw RequestValidationError(
                field + " must contain only strings");
        }
        values.push_back(value.asString());
    }
    return values;
}

std::string compact_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

labbridge::core::SourceType parse_source_type(const std::string& value) {
    if (value == "local_directory") {
        return labbridge::core::SourceType::LocalDirectory;
    }
    if (value == "ftp") {
        return labbridge::core::SourceType::Ftp;
    }
    if (value == "oracle") {
        return labbridge::core::SourceType::Oracle;
    }
    throw RequestValidationError("unsupported source_type");
}

ManagementDataSourceCreateRequest parse_data_source_create(
    const Json::Value& body) {
    require_allowed_members(
        body, {"node_code", "source_type", "name", "config", "enabled"});

    ManagementDataSourceCreateRequest request;
    request.node_code = required_string(body, "node_code");
    request.source_type = parse_source_type(
        required_string(body, "source_type"));
    request.name = required_string(body, "name");
    request.config_json = compact_json(required_object(body, "config"));
    request.enabled = required_bool(body, "enabled");
    return request;
}

ManagementQcRuleCreateRequest parse_qc_rule_create(
    const Json::Value& body) {
    require_allowed_members(
        body, {"name", "rule_type", "config", "enabled"});

    ManagementQcRuleCreateRequest request;
    request.name = required_string(body, "name");
    request.rule_type = required_string(body, "rule_type");
    request.rule_config_json = compact_json(required_object(body, "config"));
    request.enabled = required_bool(body, "enabled");
    return request;
}

ManagementTaskCreateRequest parse_task_create(const Json::Value& body) {
    require_allowed_members(
        body,
        {"node_code", "data_source_id", "name", "task_type",
         "schedule_expr", "parser_type", "qc_profile", "qc_rule_ids",
         "enabled"});

    ManagementTaskCreateRequest request;
    request.node_code = required_string(body, "node_code");
    request.data_source_id = required_string(body, "data_source_id");
    request.name = required_string(body, "name");
    request.task_type = required_string(body, "task_type");
    request.schedule_expr = required_string(body, "schedule_expr");
    request.parser_type = required_string(body, "parser_type");
    request.qc_profile = required_string(body, "qc_profile");
    request.qc_rule_ids = required_string_array(body, "qc_rule_ids");
    request.enabled = required_bool(body, "enabled");
    return request;
}

bool parse_task_enabled_patch(const Json::Value& body) {
    require_allowed_members(body, {"enabled"});
    return required_bool(body, "enabled");
}

std::optional<std::string> optional_parameter(
    const Parameters& parameters,
    const std::string& name) {
    const auto found = parameters.find(name);
    return found == parameters.end()
        ? std::nullopt
        : std::optional<std::string>{found->second};
}

std::string required_parameter(const Parameters& parameters,
                               const std::string& name) {
    const auto value = optional_parameter(parameters, name);
    if (!value.has_value() || value->empty()) {
        throw RequestValidationError(name + " is required");
    }
    return *value;
}

std::optional<bool> optional_bool(const Parameters& parameters,
                                  const std::string& name) {
    const auto value = optional_parameter(parameters, name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value == "true") {
        return true;
    }
    if (*value == "false") {
        return false;
    }
    throw RequestValidationError(name + " must be true or false");
}

PageInput page_input(const Parameters& parameters) {
    PageInput page;
    if (const auto limit = optional_parameter(parameters, "limit")) {
        int parsed = 0;
        const auto result = std::from_chars(
            limit->data(), limit->data() + limit->size(), parsed);
        if (limit->empty() || result.ec != std::errc{} ||
            result.ptr != limit->data() + limit->size()) {
            throw RequestValidationError("limit must be an integer");
        }
        page.limit = parsed;
    }
    page.cursor = optional_parameter(parameters, "cursor");
    return page;
}

std::string node_status(labbridge::core::NodeStatus status) {
    return status == labbridge::core::NodeStatus::Online ? "online" : "offline";
}

std::string run_status(labbridge::core::TaskRunStatus status) {
    switch (status) {
        case labbridge::core::TaskRunStatus::Pending:
            return "pending";
        case labbridge::core::TaskRunStatus::Running:
            return "running";
        case labbridge::core::TaskRunStatus::Succeeded:
            return "succeeded";
        case labbridge::core::TaskRunStatus::Failed:
            return "failed";
    }
    throw std::runtime_error("unsupported task run status");
}

std::string source_type(labbridge::core::SourceType type) {
    switch (type) {
        case labbridge::core::SourceType::LocalDirectory:
            return "local_directory";
        case labbridge::core::SourceType::Ftp:
            return "ftp";
        case labbridge::core::SourceType::Oracle:
            return "oracle";
    }
    throw std::runtime_error("unsupported data source type");
}

Json::Value nullable_string(const std::string& value) {
    return value.empty() ? Json::Value{} : Json::Value{value};
}

Json::Value stored_json_object(const std::string& text,
                               const std::string& field) {
    Json::CharReaderBuilder builder;
    auto reader = std::unique_ptr<Json::CharReader>{builder.newCharReader()};
    Json::Value value;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors) ||
        !value.isObject()) {
        throw std::runtime_error(field + " must contain a JSON object");
    }
    return value;
}

Json::Value node_json(const ManagementNode& node) {
    Json::Value value;
    value["id"] = node.record.id;
    value["node_code"] = node.record.info.node_code;
    value["name"] = node.record.info.name;
    value["agent_version"] = node.record.info.agent_version;
    value["stored_status"] = node_status(node.record.status);
    value["effective_status"] = node_status(node.effective_status);
    value["last_heartbeat_at"] = nullable_string(node.record.last_heartbeat_at);
    value["created_at"] = nullable_string(node.record.created_at);
    value["updated_at"] = nullable_string(node.record.updated_at);
    return value;
}

Json::Value task_run_record_json(const TaskRunRecord& run) {
    Json::Value value;
    value["id"] = run.id;
    value["task_id"] = run.task_id;
    value["node_code"] = run.node_code;
    value["status"] = run_status(run.status);
    value["started_at"] = nullable_string(run.started_at);
    value["finished_at"] = nullable_string(run.finished_at);
    value["scheduled_for"] = nullable_string(run.scheduled_for);
    value["trigger_type"] = run.trigger_type;
    value["execution_key"] = nullable_string(run.execution_key);
    value["items_total"] = run.items_total;
    value["items_success"] = run.items_success;
    value["items_failed"] = run.items_failed;
    value["error_summary"] = nullable_string(run.error_summary);
    return value;
}

Json::Value node_summary_json(const ManagementNodeSummary& summary) {
    Json::Value value = node_json({summary.record.node,
                                   summary.effective_status});
    value["enabled_task_count"] = summary.record.enabled_task_count;
    value["disabled_task_count"] = summary.record.disabled_task_count;
    value["open_alert_count"] = summary.record.open_alert_count;
    value["latest_task_run"] = Json::Value{};
    if (summary.record.latest_task_run.has_value()) {
        value["latest_task_run"] =
            task_run_record_json(*summary.record.latest_task_run);
    }
    return value;
}

Json::Value data_source_json(const DataSourceRecord& record) {
    Json::Value value;
    value["id"] = record.id;
    value["node_code"] = record.node_code;
    value["source_type"] = source_type(record.source_type);
    value["name"] = record.name;
    value["config"] = stored_json_object(record.config_json, "data source config");
    value["enabled"] = record.enabled;
    value["created_at"] = nullable_string(record.created_at);
    value["updated_at"] = nullable_string(record.updated_at);
    return value;
}

Json::Value qc_rule_json(const QcRuleRecord& record) {
    Json::Value value;
    value["id"] = record.id;
    value["name"] = record.name;
    value["rule_type"] = record.rule_type;
    value["config"] = stored_json_object(record.rule_config_json, "QC rule config");
    value["enabled"] = record.enabled;
    value["created_at"] = nullable_string(record.created_at);
    return value;
}

Json::Value task_json(const TaskRecord& record) {
    Json::Value value;
    value["id"] = record.id;
    value["node_code"] = record.node_code;
    value["data_source_id"] = record.data_source_id;
    value["name"] = record.name;
    value["task_type"] = record.task_type;
    value["schedule_expr"] = record.schedule_expr;
    value["parser_type"] = record.parser_type;
    value["qc_profile"] = record.qc_profile;
    value["enabled"] = record.enabled;
    value["qc_rule_ids"] = Json::Value{Json::arrayValue};
    for (const auto& id : record.qc_rule_ids) {
        value["qc_rule_ids"].append(id);
    }
    value["created_at"] = nullable_string(record.created_at);
    value["updated_at"] = nullable_string(record.updated_at);
    return value;
}

Json::Value task_run_json(const ManagementTaskRun& run) {
    auto value = task_run_record_json(run.record);
    value["stale"] = run.stale;
    value["stale_after_seconds"] = run.stale_after_seconds;
    return value;
}

Json::Value task_run_summary_json(const ManagementTaskRunSummary& summary) {
    auto value = task_run_json({summary.record.task_run,
                                summary.stale,
                                summary.stale_after_seconds});
    value["raw_file_count"] = summary.record.raw_file_count;
    value["parsed_record_count"] = summary.record.parsed_record_count;
    value["qc_result_count"] = summary.record.qc_result_count;
    value["alert_count"] = summary.record.alert_count;
    return value;
}

Json::Value raw_file_json(const RawFileRecord& record) {
    Json::Value value;
    value["id"] = record.id;
    value["task_run_id"] = record.task_run_id;
    value["node_code"] = record.node_code;
    value["original_name"] = record.original_name;
    value["file_hash"] = record.file_hash;
    value["storage_path"] = record.storage_path;
    value["size_bytes"] = Json::Int64{record.size_bytes};
    value["source_mtime"] = nullable_string(record.source_mtime);
    value["ingest_status"] = record.ingest_status;
    value["created_at"] = nullable_string(record.created_at);
    return value;
}

Json::Value parsed_record_json(const ParsedRecordRecord& record) {
    Json::Value value;
    value["id"] = record.id;
    value["raw_file_id"] = record.raw_file_id;
    value["task_run_id"] = record.task_run_id;
    value["station_code"] = record.record.station_code;
    value["device_code"] = record.record.device_code;
    value["record_time"] = record.record.record_time;
    value["payload"] = stored_json_object(record.record.payload_json, "payload");
    value["parse_status"] = record.parse_status;
    value["created_at"] = nullable_string(record.created_at);
    return value;
}

Json::Value qc_result_json(const QcResultRecord& record) {
    Json::Value value;
    value["id"] = record.id;
    value["task_run_id"] = record.task_run_id;
    value["parsed_record_id"] = record.parsed_record_id;
    value["qc_rule_id"] = record.qc_rule_id;
    value["level"] = record.level;
    value["result"] = record.result;
    value["message"] = record.message;
    value["created_at"] = nullable_string(record.created_at);
    return value;
}

Json::Value alert_json(const AlertRecord& record) {
    Json::Value value;
    value["id"] = record.id;
    value["node_code"] = record.node_code;
    value["task_run_id"] = record.task_run_id;
    value["alert_type"] = record.alert_type;
    value["severity"] = record.severity;
    value["message"] = record.message;
    value["status"] = record.status;
    value["created_at"] = nullable_string(record.created_at);
    return value;
}

template <typename Record, typename Mapper>
void respond_command(const ManagementCommandResult& result,
                     drogon::HttpStatusCode status,
                     Mapper mapper,
                     http::ResponseCallback& callback) {
    if (!result.status.ok) {
        callback(http::status_error_response(result.status));
        return;
    }
    const auto* item = std::get_if<Record>(&result.item);
    if (item == nullptr) {
        throw std::runtime_error(
            "management command result is missing its response item");
    }
    callback(http::success_response(status, mapper(*item)));
}

template <typename T, typename Mapper>
Json::Value page_json(const ManagementPage<T>& page, Mapper mapper) {
    Json::Value value;
    value["items"] = Json::Value{Json::arrayValue};
    for (const auto& item : page.items) {
        value["items"].append(mapper(item));
    }
    value["next_cursor"] = page.next_cursor.has_value()
        ? Json::Value{*page.next_cursor}
        : Json::Value{};
    value["has_more"] = page.has_more;
    return value;
}

template <typename Result, typename Mapper>
void respond_page(const Result& result,
                  Mapper mapper,
                  http::ResponseCallback& callback) {
    if (!result.status.ok) {
        callback(http::status_error_response(result.status));
        return;
    }
    callback(http::success_response(
        drogon::k200OK, page_json(result.page, mapper)));
}

template <typename Result, typename Mapper>
void respond_item(const Result& result,
                  Mapper mapper,
                  http::ResponseCallback& callback) {
    if (!result.status.ok) {
        callback(http::status_error_response(result.status));
        return;
    }
    if (!result.item.has_value()) {
        throw std::runtime_error("management query result is missing item");
    }
    callback(http::success_response(drogon::k200OK, mapper(*result.item)));
}

template <typename Operation>
void handle_request(std::string_view route,
                    Operation operation,
                    http::ResponseCallback& callback) {
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
        callback(http::error_response(
            drogon::k400BadRequest, "invalid_argument", error.what()));
    } catch (const std::exception&) {
        http::handle_unknown_exception(kComponent, callback);
    } catch (...) {
        http::handle_unknown_exception(kComponent, callback);
    }

    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    const int status = emitted_response == nullptr
        ? 0
        : static_cast<int>(emitted_response->statusCode());
    std::string message = std::string{route} +
        " status=" + std::to_string(status) +
        " duration_ms=" + std::to_string(duration.count());
    if (emitted_response != nullptr) {
        const auto& json = emitted_response->getJsonObject();
        if (json != nullptr && (*json)["ok"].asBool() &&
            (*json)["data"]["items"].isArray()) {
            message += " items=" + std::to_string(
                (*json)["data"]["items"].size());
            message += " has_more=" + std::string{
                (*json)["data"]["has_more"].asBool() ? "true" : "false"};
        } else if (json != nullptr && (*json)["ok"].asBool() &&
                   (*json)["data"]["id"].isString()) {
            message += " id=" + (*json)["data"]["id"].asString();
            if ((*json)["data"]["enabled"].isBool()) {
                message += " enabled=" + std::string{
                    (*json)["data"]["enabled"].asBool()
                        ? "true" : "false"};
            }
        }
    }
    labbridge::core::log_info(kComponent, message);
}

}  // namespace

ManagementHttpController::ManagementHttpController(
    ManagementQueryHandlers query_handlers,
    ManagementCommandHandlers command_handlers)
    : query_handlers_(std::move(query_handlers)),
      command_handlers_(std::move(command_handlers)) {
    if (!query_handlers_.list_nodes || !query_handlers_.find_node ||
        !query_handlers_.list_data_sources || !query_handlers_.list_qc_rules ||
        !query_handlers_.list_tasks || !query_handlers_.list_task_runs ||
        !query_handlers_.find_task_run || !query_handlers_.list_raw_files ||
        !query_handlers_.list_parsed_records || !query_handlers_.list_qc_results ||
        !query_handlers_.list_alerts) {
        throw std::invalid_argument("management query HTTP handlers are required");
    }
    if (!command_handlers_.create_data_source ||
        !command_handlers_.create_qc_rule || !command_handlers_.create_task ||
        !command_handlers_.set_task_enabled) {
        throw std::invalid_argument(
            "management command HTTP handlers are required");
    }
}

void ManagementHttpController::register_routes(drogon::HttpAppFramework& app) {
    auto self = shared_from_this();
    app.registerHandler(
        "/api/v1/nodes",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_nodes(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/nodes/{1}",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback,
               const std::string& node_code) {
            self->get_node(request, node_code, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/data-sources",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_data_sources(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/qc-rules",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_qc_rules(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/tasks",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_tasks(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/task-runs",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_task_runs(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/task-runs/{1}",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback,
               const std::string& task_run_id) {
            self->get_task_run(request, task_run_id, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/raw-files",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_raw_files(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/parsed-records",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_parsed_records(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/qc-results",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_qc_results(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/alerts",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->get_alerts(request, std::move(callback));
        },
        {drogon::Get});
    app.registerHandler(
        "/api/v1/data-sources",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->post_data_source(request, std::move(callback));
        },
        {drogon::Post});
    app.registerHandler(
        "/api/v1/qc-rules",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->post_qc_rule(request, std::move(callback));
        },
        {drogon::Post});
    app.registerHandler(
        "/api/v1/tasks",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback) {
            self->post_task(request, std::move(callback));
        },
        {drogon::Post});
    app.registerHandler(
        "/api/v1/tasks/{1}",
        [self](const drogon::HttpRequestPtr& request,
               ResponseCallback&& callback,
               const std::string& task_id) {
            self->patch_task(request, task_id, std::move(callback));
        },
        {drogon::Patch});
}

void ManagementHttpController::get_nodes(const drogon::HttpRequestPtr& request,
                                         ResponseCallback&& callback) const {
    handle_request("GET /api/v1/nodes", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters, {"status", "limit", "cursor"});
        respond_page(query_handlers_.list_nodes({optional_parameter(parameters, "status"),
                                           page_input(parameters)}),
                     node_json, callback);
    }, callback);
}

void ManagementHttpController::get_node(const drogon::HttpRequestPtr& request,
                                        const std::string& node_code,
                                        ResponseCallback&& callback) const {
    handle_request("GET /api/v1/nodes/{nodeCode}", [&] {
        require_allowed_parameters(request->getParameters(), {});
        respond_item(query_handlers_.find_node(node_code), node_summary_json, callback);
    }, callback);
}

void ManagementHttpController::get_data_sources(
    const drogon::HttpRequestPtr& request, ResponseCallback&& callback) const {
    handle_request("GET /api/v1/data-sources", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters, {"node_code", "enabled", "limit", "cursor"});
        respond_page(query_handlers_.list_data_sources({required_parameter(parameters, "node_code"),
                                                  optional_bool(parameters, "enabled"),
                                                  page_input(parameters)}),
                     data_source_json, callback);
    }, callback);
}

void ManagementHttpController::get_qc_rules(const drogon::HttpRequestPtr& request,
                                            ResponseCallback&& callback) const {
    handle_request("GET /api/v1/qc-rules", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters, {"enabled", "limit", "cursor"});
        respond_page(query_handlers_.list_qc_rules({optional_bool(parameters, "enabled"),
                                              page_input(parameters)}),
                     qc_rule_json, callback);
    }, callback);
}

void ManagementHttpController::get_tasks(const drogon::HttpRequestPtr& request,
                                         ResponseCallback&& callback) const {
    handle_request("GET /api/v1/tasks", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters, {"node_code", "enabled", "limit", "cursor"});
        respond_page(query_handlers_.list_tasks({required_parameter(parameters, "node_code"),
                                           optional_bool(parameters, "enabled"),
                                           page_input(parameters)}),
                     task_json, callback);
    }, callback);
}

void ManagementHttpController::get_task_runs(const drogon::HttpRequestPtr& request,
                                             ResponseCallback&& callback) const {
    handle_request("GET /api/v1/task-runs", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters,
            {"node_code", "task_id", "status", "limit", "cursor"});
        respond_page(query_handlers_.list_task_runs({required_parameter(parameters, "node_code"),
                                               optional_parameter(parameters, "task_id"),
                                               optional_parameter(parameters, "status"),
                                               page_input(parameters)}),
                     task_run_json, callback);
    }, callback);
}

void ManagementHttpController::get_task_run(const drogon::HttpRequestPtr& request,
                                            const std::string& task_run_id,
                                            ResponseCallback&& callback) const {
    handle_request("GET /api/v1/task-runs/{runId}", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters, {"node_code"});
        respond_item(query_handlers_.find_task_run(required_parameter(parameters, "node_code"),
                                              task_run_id),
                     task_run_summary_json, callback);
    }, callback);
}

void ManagementHttpController::get_raw_files(const drogon::HttpRequestPtr& request,
                                             ResponseCallback&& callback) const {
    handle_request("GET /api/v1/raw-files", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters, {"task_run_id", "limit", "cursor"});
        respond_page(query_handlers_.list_raw_files({required_parameter(parameters, "task_run_id"),
                                               page_input(parameters)}),
                     raw_file_json, callback);
    }, callback);
}

void ManagementHttpController::get_parsed_records(
    const drogon::HttpRequestPtr& request, ResponseCallback&& callback) const {
    handle_request("GET /api/v1/parsed-records", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters, {"task_run_id", "limit", "cursor"});
        respond_page(query_handlers_.list_parsed_records(
                         {required_parameter(parameters, "task_run_id"),
                          page_input(parameters)}),
                     parsed_record_json, callback);
    }, callback);
}

void ManagementHttpController::get_qc_results(const drogon::HttpRequestPtr& request,
                                              ResponseCallback&& callback) const {
    handle_request("GET /api/v1/qc-results", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters, {"task_run_id", "result", "limit", "cursor"});
        respond_page(query_handlers_.list_qc_results({required_parameter(parameters, "task_run_id"),
                                                optional_parameter(parameters, "result"),
                                                page_input(parameters)}),
                     qc_result_json, callback);
    }, callback);
}

void ManagementHttpController::get_alerts(const drogon::HttpRequestPtr& request,
                                          ResponseCallback&& callback) const {
    handle_request("GET /api/v1/alerts", [&] {
        const auto& parameters = request->getParameters();
        require_allowed_parameters(parameters,
            {"node_code", "task_run_id", "status", "severity", "limit", "cursor"});
        respond_page(query_handlers_.list_alerts({required_parameter(parameters, "node_code"),
                                            optional_parameter(parameters, "task_run_id"),
                                            optional_parameter(parameters, "status"),
                                            optional_parameter(parameters, "severity"),
                                            page_input(parameters)}),
                     alert_json, callback);
    }, callback);
}

void ManagementHttpController::post_data_source(
    const drogon::HttpRequestPtr& request,
    ResponseCallback&& callback) const {
    handle_request("POST /api/v1/data-sources", [&] {
        require_allowed_parameters(request->getParameters(), {});
        if (!http::require_json_content_type(request, callback)) {
            return;
        }
        respond_command<DataSourceRecord>(
            command_handlers_.create_data_source(
                parse_data_source_create(parse_json_body(request))),
            drogon::k201Created,
            data_source_json,
            callback);
    }, callback);
}

void ManagementHttpController::post_qc_rule(
    const drogon::HttpRequestPtr& request,
    ResponseCallback&& callback) const {
    handle_request("POST /api/v1/qc-rules", [&] {
        require_allowed_parameters(request->getParameters(), {});
        if (!http::require_json_content_type(request, callback)) {
            return;
        }
        respond_command<QcRuleRecord>(
            command_handlers_.create_qc_rule(
                parse_qc_rule_create(parse_json_body(request))),
            drogon::k201Created,
            qc_rule_json,
            callback);
    }, callback);
}

void ManagementHttpController::post_task(
    const drogon::HttpRequestPtr& request,
    ResponseCallback&& callback) const {
    handle_request("POST /api/v1/tasks", [&] {
        require_allowed_parameters(request->getParameters(), {});
        if (!http::require_json_content_type(request, callback)) {
            return;
        }
        respond_command<TaskRecord>(
            command_handlers_.create_task(
                parse_task_create(parse_json_body(request))),
            drogon::k201Created,
            task_json,
            callback);
    }, callback);
}

void ManagementHttpController::patch_task(
    const drogon::HttpRequestPtr& request,
    const std::string& task_id,
    ResponseCallback&& callback) const {
    handle_request("PATCH /api/v1/tasks/{taskId}", [&] {
        require_allowed_parameters(request->getParameters(), {});
        if (!http::require_json_content_type(request, callback)) {
            return;
        }
        respond_command<TaskRecord>(
            command_handlers_.set_task_enabled(
                task_id,
                parse_task_enabled_patch(parse_json_body(request))),
            drogon::k200OK,
            task_json,
            callback);
    }, callback);
}

}  // namespace labbridge::server
