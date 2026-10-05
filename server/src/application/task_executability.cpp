#include "labbridge/server/application/task_executability.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <optional>

namespace labbridge::server {
namespace {

using labbridge::core::Status;
using labbridge::core::StatusCode;
using Json = nlohmann::json;

Status not_found(std::string message) {
    return Status::failure(StatusCode::NotFound, std::move(message));
}

Status conflict(std::string message) {
    return Status::failure(StatusCode::Conflict, std::move(message));
}

bool is_supported_rule_type(const std::string& rule_type) {
    return rule_type == "required_fields" ||
           rule_type == "basic_timestamp_format";
}

bool is_blank(const std::string& value) {
    for (const unsigned char character : value) {
        if (!std::isspace(character)) {
            return false;
        }
    }
    return true;
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

}  // namespace

Status validate_task_executable(
    INodeRepository& node_repository,
    IConfigRepository& config_repository,
    IQcRepository& qc_repository,
    const TaskRecord& task,
    const std::vector<std::string>& qc_rule_ids) {
    if (!node_repository.find_by_code(task.node_code).has_value()) {
        return not_found("node is not found");
    }
    const auto data_source =
        config_repository.find_data_source(task.data_source_id);
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
    for (const auto& qc_rule_id : qc_rule_ids) {
        const auto rule = qc_repository.find_rule(qc_rule_id);
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
