#include "labbridge/server/application/management_command_service.h"

#include "labbridge/core/cron_schedule.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <charconv>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace labbridge::server {
namespace {

using labbridge::core::Status;
using labbridge::core::StatusCode;
using Json = nlohmann::json;

constexpr std::size_t kMaximumNameLength = 128;
constexpr std::size_t kMaximumNodeCodeLength = 64;
constexpr std::size_t kMaximumScheduleLength = 128;
constexpr std::size_t kMaximumQcProfileLength = 64;
constexpr std::size_t kMaximumQcRulesPerTask = 5;

Status invalid(std::string message) {
    return Status::failure(StatusCode::InvalidArgument, std::move(message));
}

Status not_found(std::string message) {
    return Status::failure(StatusCode::NotFound, std::move(message));
}

Status conflict(std::string message) {
    return Status::failure(StatusCode::Conflict, std::move(message));
}

bool is_blank(const std::string& value) {
    for (const unsigned char character : value) {
        if (!std::isspace(character)) {
            return false;
        }
    }
    return true;
}

bool is_positive_id(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    unsigned long long parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} &&
           result.ptr == value.data() + value.size() &&
           parsed > 0 &&
           parsed <= static_cast<unsigned long long>(
               std::numeric_limits<long long>::max());
}

bool is_supported_rule_type(const std::string& rule_type) {
    return rule_type == "required_fields" ||
           rule_type == "basic_timestamp_format";
}

Status validate_name(const std::string& name, const std::string& field) {
    if (is_blank(name)) {
        return invalid(field + " is required");
    }
    if (name.size() > kMaximumNameLength) {
        return invalid(field + " must not exceed 128 characters");
    }
    return Status::success();
}

std::optional<Json> parse_json_object(const std::string& text) {
    try {
        auto value = Json::parse(text);
        if (!value.is_object()) {
            return std::nullopt;
        }
        return value;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

template <typename Record>
ManagementCommandResult successful_result(std::string id,
                                          std::optional<Record> record) {
    // 写入对象必须在同一事务内可读，否则说明 repository 契约已被破坏。
    if (!record.has_value()) {
        throw std::runtime_error("created management object is not readable");
    }
    return {Status::success(), std::move(id), std::move(*record)};
}

}  // namespace

ManagementCommandService::ManagementCommandService(
    INodeRepository& node_repository,
    IConfigRepository& config_repository,
    IQcRepository& qc_repository)
    : node_repository_(node_repository),
      config_repository_(config_repository),
      qc_repository_(qc_repository) {}

ManagementCommandResult ManagementCommandService::create_data_source(
    const ManagementDataSourceCreateRequest& request) {
    if (is_blank(request.node_code) ||
        request.node_code.size() > kMaximumNodeCodeLength) {
        return {invalid("node_code must contain 1 to 64 characters"), {}};
    }
    if (request.source_type != labbridge::core::SourceType::LocalDirectory) {
        return {invalid("source_type must be local_directory"), {}};
    }
    const auto name_status = validate_name(request.name, "data source name");
    if (!name_status.ok) {
        return {name_status, {}};
    }

    const auto config = parse_json_object(request.config_json);
    if (!config.has_value() || !config->contains("root_path") ||
        !config->at("root_path").is_string() ||
        is_blank(config->at("root_path").get<std::string>()) ||
        !config->contains("extension") ||
        !config->at("extension").is_string()) {
        return {invalid(
                    "data source config requires non-empty string root_path "
                    "and extension"),
                {}};
    }
    const auto extension = config->at("extension").get<std::string>();
    if (extension.empty() || extension.front() != '.') {
        return {invalid("data source extension must start with '.'"), {}};
    }
    if (!node_repository_.find_by_code(request.node_code).has_value()) {
        return {not_found("node is not found"), {}};
    }

    DataSourceRecord record;
    record.node_code = request.node_code;
    record.source_type = request.source_type;
    record.name = request.name;
    record.config_json = request.config_json;
    record.enabled = request.enabled;
    const auto id = config_repository_.create_data_source(std::move(record));
    return successful_result(
        id, config_repository_.find_data_source(id));
}

ManagementCommandResult ManagementCommandService::create_qc_rule(
    const ManagementQcRuleCreateRequest& request) {
    const auto name_status = validate_name(request.name, "QC rule name");
    if (!name_status.ok) {
        return {name_status, {}};
    }
    if (!is_supported_rule_type(request.rule_type)) {
        return {invalid("unsupported QC rule type"), {}};
    }
    const auto config = parse_json_object(request.rule_config_json);
    if (!config.has_value() || !config->empty()) {
        return {invalid("QC rule config must be an empty object"), {}};
    }

    QcRuleRecord record;
    record.name = request.name;
    record.rule_type = request.rule_type;
    record.rule_config_json = request.rule_config_json;
    record.enabled = request.enabled;
    const auto id = qc_repository_.create_rule(std::move(record));
    return successful_result(id, qc_repository_.find_rule(id));
}

ManagementCommandResult ManagementCommandService::create_task(
    const ManagementTaskCreateRequest& request) {
    if (is_blank(request.node_code) ||
        request.node_code.size() > kMaximumNodeCodeLength) {
        return {invalid("node_code must contain 1 to 64 characters"), {}};
    }
    if (!is_positive_id(request.data_source_id)) {
        return {invalid("data_source_id must be a positive integer"), {}};
    }
    const auto name_status = validate_name(request.name, "task name");
    if (!name_status.ok) {
        return {name_status, {}};
    }
    if (request.task_type != "local_file_import") {
        return {invalid("task_type must be local_file_import"), {}};
    }
    if (request.parser_type != "csv_observation") {
        return {invalid("parser_type must be csv_observation"), {}};
    }
    if (request.schedule_expr.empty() ||
        request.schedule_expr.size() > kMaximumScheduleLength) {
        return {invalid("schedule_expr must contain 1 to 128 characters"), {}};
    }
    try {
        static_cast<void>(
            labbridge::core::CronSchedule::parse(request.schedule_expr));
    } catch (const std::invalid_argument& error) {
        return {invalid(error.what()), {}};
    }
    if (request.qc_profile.size() > kMaximumQcProfileLength) {
        return {invalid("qc_profile must not exceed 64 characters"), {}};
    }
    if (request.qc_rule_ids.empty() ||
        request.qc_rule_ids.size() > kMaximumQcRulesPerTask) {
        return {invalid("qc_rule_ids must contain 1 to 5 IDs"), {}};
    }

    std::unordered_set<std::string> unique_rule_ids;
    for (const auto& qc_rule_id : request.qc_rule_ids) {
        if (!is_positive_id(qc_rule_id)) {
            return {invalid("qc_rule_ids must contain positive integers"), {}};
        }
        if (!unique_rule_ids.insert(qc_rule_id).second) {
            return {invalid("qc_rule_ids must not contain duplicates"), {}};
        }
    }

    TaskRecord task;
    task.node_code = request.node_code;
    task.data_source_id = request.data_source_id;
    task.name = request.name;
    task.task_type = request.task_type;
    task.schedule_expr = request.schedule_expr;
    task.parser_type = request.parser_type;
    task.qc_profile = request.qc_profile;
    task.enabled = request.enabled;

    const auto dependency_status =
        validate_task_dependencies(task, request.qc_rule_ids);
    if (!dependency_status.ok) {
        return {dependency_status, {}};
    }

    const auto task_id = config_repository_.create_task(std::move(task));
    // 绑定顺序属于任务配置语义，使用固定步长为后续插入规则保留空间。
    for (std::size_t index = 0; index < request.qc_rule_ids.size(); ++index) {
        config_repository_.bind_task_qc_rule(
            task_id,
            request.qc_rule_ids[index],
            static_cast<int>((index + 1) * 10));
    }
    auto created_task = config_repository_.find_task(task_id);
    if (created_task.has_value()) {
        created_task->qc_rule_ids =
            config_repository_.find_task_qc_rule_ids(task_id);
    }
    return successful_result(task_id, std::move(created_task));
}

ManagementCommandResult ManagementCommandService::set_task_enabled(
    const std::string& task_id,
    bool enabled) {
    if (!is_positive_id(task_id)) {
        return {invalid("task_id must be a positive integer"), {}};
    }
    const auto task = config_repository_.find_task(task_id);
    if (!task.has_value()) {
        return {not_found("task is not found"), {}};
    }

    if (enabled) {
        const auto rule_ids =
            config_repository_.find_task_qc_rule_ids(task_id);
        const auto dependency_status =
            validate_task_dependencies(*task, rule_ids);
        if (!dependency_status.ok) {
            return {dependency_status, {}};
        }
    }

    // 禁用仅改变后续中心端投影，不触碰 Agent 已持久化或运行中的作业。
    config_repository_.set_task_enabled(task_id, enabled);
    auto updated_task = config_repository_.find_task(task_id);
    if (updated_task.has_value()) {
        updated_task->qc_rule_ids =
            config_repository_.find_task_qc_rule_ids(task_id);
    }
    return successful_result(task_id, std::move(updated_task));
}

Status ManagementCommandService::validate_task_dependencies(
    const TaskRecord& task,
    const std::vector<std::string>& qc_rule_ids) const {
    if (!node_repository_.find_by_code(task.node_code).has_value()) {
        return not_found("node is not found");
    }
    const auto data_source =
        config_repository_.find_data_source(task.data_source_id);
    if (!data_source.has_value()) {
        return not_found("data source is not found");
    }
    if (data_source->node_code != task.node_code) {
        return conflict("data source does not belong to node");
    }
    if (!data_source->enabled) {
        return conflict("data source is disabled");
    }
    if (data_source->source_type !=
        labbridge::core::SourceType::LocalDirectory) {
        return conflict("data source type is not executable");
    }
    const auto source_config = parse_json_object(data_source->config_json);
    if (!source_config.has_value() ||
        !source_config->contains("root_path") ||
        !source_config->at("root_path").is_string() ||
        is_blank(source_config->at("root_path").get<std::string>()) ||
        !source_config->contains("extension") ||
        !source_config->at("extension").is_string() ||
        source_config->at("extension").get<std::string>() != ".csv") {
        return conflict("data source config is not executable by CSV tasks");
    }
    if (task.task_type != "local_file_import" ||
        task.parser_type != "csv_observation") {
        return conflict("task type or parser is not executable");
    }
    try {
        static_cast<void>(
            labbridge::core::CronSchedule::parse(task.schedule_expr));
    } catch (const std::invalid_argument&) {
        return conflict("task schedule is not executable");
    }
    if (qc_rule_ids.empty() || qc_rule_ids.size() > kMaximumQcRulesPerTask) {
        return conflict("task must bind 1 to 5 QC rules");
    }
    for (const auto& qc_rule_id : qc_rule_ids) {
        const auto rule = qc_repository_.find_rule(qc_rule_id);
        if (!rule.has_value()) {
            return not_found("QC rule is not found");
        }
        if (!rule->enabled) {
            return conflict("QC rule is disabled");
        }
        if (!is_supported_rule_type(rule->rule_type)) {
            return conflict("QC rule type is not executable");
        }
        const auto rule_config = parse_json_object(rule->rule_config_json);
        if (!rule_config.has_value() || !rule_config->empty()) {
            return conflict("QC rule config is not executable");
        }
    }
    return Status::success();
}

}  // namespace labbridge::server
