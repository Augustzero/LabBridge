#include "labbridge/server/application/config_service.h"

#include "labbridge/core/logging.h"

#include <unordered_map>
#include <unordered_set>

#include <utility>

namespace labbridge::server {

ConfigService::ConfigService(INodeRepository& node_repository, IConfigRepository& config_repository)
    : node_repository_(node_repository), config_repository_(config_repository) {}

ConfigCreateResult ConfigService::create_data_source(const CreateDataSourceRequest& request) {
    if (request.node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"), {}};
    }
    if (request.name.empty()) {
        return {labbridge::core::Status::failure("data source name is required"), {}};
    }
    if (request.config_json.empty()) {
        return {labbridge::core::Status::failure("data source config_json is required"), {}};
    }
    if (!node_repository_.find_by_code(request.node_code).has_value()) {
        return {labbridge::core::Status::failure("node is not registered"), {}};
    }

    DataSourceRecord record;
    record.node_code = request.node_code;
    record.source_type = request.source_type;
    record.name = request.name;
    record.config_json = request.config_json;
    record.enabled = request.enabled;

    const auto id = config_repository_.create_data_source(std::move(record));
    return {labbridge::core::Status::success(), id};
}

ConfigCreateResult ConfigService::create_task(const CreateTaskRequest& request) {
    if (request.node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"), {}};
    }
    if (request.data_source_id.empty()) {
        return {labbridge::core::Status::failure("data_source_id is required"), {}};
    }
    if (request.name.empty()) {
        return {labbridge::core::Status::failure("task name is required"), {}};
    }
    if (request.task_type.empty()) {
        return {labbridge::core::Status::failure("task_type is required"), {}};
    }
    if (request.schedule_expr.empty()) {
        return {labbridge::core::Status::failure("schedule_expr is required"), {}};
    }
    if (request.parser_type.empty()) {
        return {labbridge::core::Status::failure("parser_type is required"), {}};
    }
    if (!node_repository_.find_by_code(request.node_code).has_value()) {
        return {labbridge::core::Status::failure("node is not registered"), {}};
    }

    const auto data_source = config_repository_.find_data_source(request.data_source_id);
    if (!data_source.has_value()) {
        return {labbridge::core::Status::failure("data source is not found"), {}};
    }
    // 任务必须绑定同节点的数据源，避免跨节点配置串用。
    if (data_source->node_code != request.node_code) {
        return {labbridge::core::Status::failure("data source does not belong to node"), {}};
    }

    TaskRecord record;
    record.node_code = request.node_code;
    record.data_source_id = request.data_source_id;
    record.name = request.name;
    record.task_type = request.task_type;
    record.schedule_expr = request.schedule_expr;
    record.parser_type = request.parser_type;
    record.qc_profile = request.qc_profile;
    record.enabled = request.enabled;

    const auto id = config_repository_.create_task(std::move(record));
    return {labbridge::core::Status::success(), id};
}

std::vector<TaskRecord> ConfigService::find_enabled_tasks(const std::string& node_code) const {
    if (node_code.empty()) {
        return {};
    }
    return config_repository_.find_enabled_tasks_by_node(node_code);
}

ExecutableConfigProjection ConfigService::find_executable_config(
    const std::string& node_code) const {
    ExecutableConfigProjection result;
    if (node_code.empty()) {
        return result;
    }

    const auto tasks =
        config_repository_.find_enabled_tasks_by_node(node_code);
    const auto data_sources =
        config_repository_.find_enabled_data_sources_by_node(node_code);
    const auto bindings =
        config_repository_.find_enabled_task_qc_rules_by_node(node_code);

    std::unordered_map<std::string, DataSourceRecord> sources_by_id;
    for (const auto& source : data_sources) {
        sources_by_id.emplace(source.id, source);
    }

    std::unordered_map<std::string, std::vector<TaskQcRuleBinding>>
        bindings_by_task;
    for (const auto& binding : bindings) {
        bindings_by_task[binding.task_id].push_back(binding);
    }

    std::unordered_set<std::string> referenced_source_ids;
    for (auto task : tasks) {
        const auto source = sources_by_id.find(task.data_source_id);
        if (source == sources_by_id.end()) {
            labbridge::core::log_error(
                "config-service",
                "skipping task_id=" + task.id +
                    "; enabled same-node data source is unavailable");
            continue;
        }

        const auto task_bindings = bindings_by_task.find(task.id);
        if (task_bindings != bindings_by_task.end()) {
            for (const auto& binding : task_bindings->second) {
                task.qc_rule_ids.push_back(binding.qc_rule_id);
                result.task_qc_rules.push_back(binding);
            }
        }
        referenced_source_ids.insert(source->first);
        result.tasks.push_back(std::move(task));
    }

    for (const auto& source : data_sources) {
        if (referenced_source_ids.count(source.id) != 0U) {
            result.data_sources.push_back(source);
        }
    }
    return result;
}

}  // namespace labbridge::server
