#include "labbridge/server/task_run_repository.h"

#include <utility>

namespace labbridge::server {

std::string InMemoryTaskRunRepository::create(TaskRunRecord task_run) {
    if (task_run.id.empty()) {
        task_run.id = std::to_string(next_task_run_id_++);
    }

    const auto id = task_run.id;
    task_runs_[id] = std::move(task_run);
    return id;
}

std::optional<TaskRunRecord> InMemoryTaskRunRepository::find_by_id(
    const std::string& task_run_id) const {
    const auto iter = task_runs_.find(task_run_id);
    if (iter == task_runs_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

void InMemoryTaskRunRepository::finish(TaskRunRecord task_run) {
    task_runs_[task_run.id] = std::move(task_run);
}

}  // namespace labbridge::server
