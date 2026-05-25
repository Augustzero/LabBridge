#include "labbridge/server/config_repository.h"

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

}  // namespace labbridge::server
