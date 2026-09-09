#include "labbridge/server/application/config_service.h"

#include "labbridge/core/logging.h"

#include <unordered_map>

#include <utility>

namespace labbridge::server {

ConfigService::ConfigService(IConfigRepository& config_repository)
    : config_repository_(config_repository) {}

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
    // SQL 已按“存在启用任务”过滤数据源，这里直接采用过滤结果。
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

    for (auto task : tasks) {
        const auto source = sources_by_id.find(task.data_source_id);
        if (source == sources_by_id.end()) {
            // 数据源被禁用或删除而任务仍启用属于配置不一致，跳过并留下线索。
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
        result.tasks.push_back(std::move(task));
    }

    // 数据源列表直接采用 SQL 的“存在启用任务”过滤结果，不再二次过滤。
    result.data_sources = std::move(data_sources);
    return result;
}

}  // namespace labbridge::server
