#include "support/server/in_memory_repositories.h"

#include <algorithm>
#include <utility>

namespace labbridge::server {
using labbridge::core::TaskRunStatus;


std::string InMemoryTaskRunRepository::create(TaskRunRecord task_run) {
    if (task_run.id.empty()) {
        task_run.id = std::to_string(next_task_run_id_++);
    }

    const auto id = task_run.id;
    task_runs_[id] = std::move(task_run);
    return id;
}

ScheduledTaskRunStart InMemoryTaskRunRepository::create_or_find_scheduled(
    TaskRunRecord task_run) {
    const auto key = task_run.node_code + "\n" + task_run.execution_key;
    const auto existing = scheduled_runs_.find(key);
    if (existing != scheduled_runs_.end()) {
        return {task_runs_.at(existing->second), false};
    }

    const auto id = create(std::move(task_run));
    scheduled_runs_[key] = id;
    return {task_runs_.at(id), true};
}

std::optional<ManualTaskRunStart>
InMemoryTaskRunRepository::create_or_find_manual(
    ManualTaskRunRequest request) {
    const auto existing = manual_runs_.find(request.execution_key);
    if (existing != manual_runs_.end()) {
        return ManualTaskRunStart{task_runs_.at(existing->second), false};
    }
    // 同任务已有 pending 人工请求时模拟部分唯一索引的行为。
    for (const auto& [id, run] : task_runs_) {
        if (run.task_id == request.task_id && run.status == TaskRunStatus::Pending &&
            (run.trigger_type == "manual" || run.trigger_type == "retry")) {
            return std::nullopt;
        }
    }

    TaskRunRecord record;
    record.task_id = request.task_id;
    record.node_code = request.node_code;
    record.status = TaskRunStatus::Pending;
    record.trigger_type = request.trigger_type;
    record.execution_key = request.execution_key;
    record.retry_of_run_id = request.retry_of_run_id;
    const auto id = create(std::move(record));
    manual_runs_[request.execution_key] = id;
    return ManualTaskRunStart{task_runs_.at(id), true};
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

std::optional<TaskRunRecord> InMemoryTaskRunRepository::find_by_execution_key(
    const std::string& execution_key) const {
    const auto iter = std::find_if(
        task_runs_.begin(), task_runs_.end(),
        [&execution_key](const auto& entry) {
            return entry.second.execution_key == execution_key;
        });
    if (iter == task_runs_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

bool InMemoryTaskRunRepository::lock_task(const std::string& task_id) {
    static_cast<void>(task_id);
    return true;
}

std::optional<TaskRunRecord> InMemoryTaskRunRepository::lock_by_id(
    const std::string& task_run_id) {
    return find_by_id(task_run_id);
}

std::optional<TaskRunRecord>
InMemoryTaskRunRepository::find_pending_manual_by_task(
    const std::string& task_id) const {
    const auto iter = std::find_if(
        task_runs_.begin(), task_runs_.end(),
        [&task_id](const auto& entry) {
            const auto& run = entry.second;
            return run.task_id == task_id && run.status == TaskRunStatus::Pending &&
                   (run.trigger_type == "manual" || run.trigger_type == "retry");
        });
    if (iter == task_runs_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

void InMemoryTaskRunRepository::fail_pending_manual_runs(
    const std::string& task_id, const std::string& error_summary) {
    for (auto& [id, run] : task_runs_) {
        if (run.task_id == task_id && run.status == TaskRunStatus::Pending &&
            (run.trigger_type == "manual" || run.trigger_type == "retry")) {
            run.status = TaskRunStatus::Failed;
            run.error_summary = error_summary;
            run.failed_files_json = "[]";
        }
    }
}

void InMemoryTaskRunRepository::mark_manual_running(
    const std::string& task_run_id, const std::string& started_at) {
    auto iter = task_runs_.find(task_run_id);
    if (iter != task_runs_.end() &&
        iter->second.status == TaskRunStatus::Pending) {
        iter->second.status = TaskRunStatus::Running;
        iter->second.started_at = started_at;
    }
}

std::vector<PendingExecutionRecord>
InMemoryTaskRunRepository::find_pending_executions_by_node(
    const std::string& node_code) const {
    std::vector<PendingExecutionRecord> executions;
    for (const auto& [id, run] : task_runs_) {
        if (run.node_code != node_code || run.status != TaskRunStatus::Pending ||
            (run.trigger_type != "manual" && run.trigger_type != "retry")) {
            continue;
        }
        PendingExecutionRecord execution;
        execution.task_run_id = run.id;
        execution.task_id = run.task_id;
        execution.execution_key = run.execution_key;
        execution.trigger_type = run.trigger_type;
        executions.push_back(std::move(execution));
    }
    std::sort(executions.begin(), executions.end(),
              [](const PendingExecutionRecord& left,
                 const PendingExecutionRecord& right) {
                  return left.task_run_id < right.task_run_id;
              });
    return executions;
}

std::optional<std::vector<TaskRunFailedFile>>
InMemoryTaskRunRepository::find_failed_files(
    const std::string& task_run_id) const {
    const auto iter = task_runs_.find(task_run_id);
    if (iter == task_runs_.end()) {
        return std::nullopt;
    }
    return parse_failed_files_json(iter->second.failed_files_json);
}

std::optional<std::vector<TaskRunRetryFile>>
InMemoryTaskRunRepository::find_retry_files(
    const std::string& task_run_id) const {
    static_cast<void>(task_run_id);
    return std::nullopt;
}

}  // namespace labbridge::server
