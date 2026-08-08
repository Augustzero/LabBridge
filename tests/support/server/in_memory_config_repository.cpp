#include "support/server/in_memory_repositories.h"

#include <algorithm>

#include <utility>

namespace labbridge::server {

std::string InMemoryConfigRepository::create_data_source(DataSourceRecord data_source) {
    if (data_source.id.empty()) {
        data_source.id = std::to_string(next_data_source_id_++);
    }

    const auto id = data_source.id;
    data_sources_[id] = std::move(data_source);
    return id;
}

std::optional<DataSourceRecord> InMemoryConfigRepository::find_data_source(
    const std::string& data_source_id) const {
    const auto iter = data_sources_.find(data_source_id);
    if (iter == data_sources_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

std::string InMemoryConfigRepository::create_task(TaskRecord task) {
    if (task.id.empty()) {
        task.id = std::to_string(next_task_id_++);
    }

    const auto id = task.id;
    tasks_[id] = std::move(task);
    return id;
}

std::optional<TaskRecord> InMemoryConfigRepository::find_task(const std::string& task_id) const {
    const auto iter = tasks_.find(task_id);
    if (iter == tasks_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

std::vector<TaskRecord> InMemoryConfigRepository::find_enabled_tasks_by_node(
    const std::string& node_code) const {
    std::vector<TaskRecord> tasks;
    for (const auto& [id, task] : tasks_) {
        if (task.node_code == node_code && task.enabled) {
            tasks.push_back(task);
        }
    }
    return tasks;
}

std::vector<DataSourceRecord>
InMemoryConfigRepository::find_enabled_data_sources_by_node(
    const std::string& node_code) const {
    std::vector<DataSourceRecord> data_sources;
    for (const auto& [id, data_source] : data_sources_) {
        if (data_source.node_code == node_code && data_source.enabled) {
            data_sources.push_back(data_source);
        }
    }
    return data_sources;
}

std::vector<TaskQcRuleBinding>
InMemoryConfigRepository::find_enabled_task_qc_rules_by_node(
    const std::string& node_code) const {
    std::vector<TaskQcRuleBinding> bindings;
    for (const auto& binding : task_qc_rules_) {
        const auto task = tasks_.find(binding.task_id);
        if (task != tasks_.end() && task->second.node_code == node_code &&
            task->second.enabled) {
            bindings.push_back(binding);
        }
    }
    std::sort(
        bindings.begin(),
        bindings.end(),
        [](const TaskQcRuleBinding& left,
           const TaskQcRuleBinding& right) {
            if (left.task_id != right.task_id) {
                return left.task_id < right.task_id;
            }
            if (left.sort_order != right.sort_order) {
                return left.sort_order < right.sort_order;
            }
            return left.qc_rule_id < right.qc_rule_id;
        });
    return bindings;
}

void InMemoryConfigRepository::bind_task_qc_rule(
    const std::string& task_id,
    const std::string& qc_rule_id,
    int sort_order) {
    task_qc_rules_.push_back({
        task_id,
        qc_rule_id,
        {},
        {},
        {},
        sort_order,
    });
}

void InMemoryConfigRepository::add_task_qc_rule_projection(
    TaskQcRuleBinding binding) {
    const auto existing = std::find_if(
        task_qc_rules_.begin(),
        task_qc_rules_.end(),
        [&binding](const TaskQcRuleBinding& candidate) {
            return candidate.task_id == binding.task_id &&
                   candidate.qc_rule_id == binding.qc_rule_id;
        });
    if (existing == task_qc_rules_.end()) {
        task_qc_rules_.push_back(std::move(binding));
        return;
    }
    *existing = std::move(binding);
}

}  // namespace labbridge::server
