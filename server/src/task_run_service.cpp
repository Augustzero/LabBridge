#include "labbridge/server/task_run_service.h"

#include <utility>

namespace labbridge::server {

TaskRunService::TaskRunService(IConfigRepository& config_repository,
                               ITaskRunRepository& task_run_repository)
    : config_repository_(config_repository), task_run_repository_(task_run_repository) {}

TaskRunCreateResult TaskRunService::start(const StartTaskRunRequest& request) {
    if (request.node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"), {}};
    }
    if (request.task_id.empty()) {
        return {labbridge::core::Status::failure("task_id is required"), {}};
    }

    const auto task = config_repository_.find_task(request.task_id);
    if (!task.has_value()) {
        return {labbridge::core::Status::failure("task is not found"), {}};
    }
    if (!task->enabled) {
        return {labbridge::core::Status::failure("task is disabled"), {}};
    }
    if (task->node_code != request.node_code) {
        return {labbridge::core::Status::failure("task does not belong to node"), {}};
    }

    TaskRunRecord record;
    record.task_id = request.task_id;
    record.node_code = request.node_code;
    record.status = labbridge::core::TaskRunStatus::Running;
    record.started_at = request.started_at;
    record.trigger_type = request.trigger_type.empty() ? "scheduled" : request.trigger_type;

    const auto id = task_run_repository_.create(std::move(record));
    return {labbridge::core::Status::success(), id};
}

labbridge::core::Status TaskRunService::finish(const FinishTaskRunRequest& request) {
    if (request.task_run_id.empty()) {
        return labbridge::core::Status::failure("task_run_id is required");
    }
    if (request.status != labbridge::core::TaskRunStatus::Succeeded &&
        request.status != labbridge::core::TaskRunStatus::Failed) {
        return labbridge::core::Status::failure("finish status must be succeeded or failed");
    }

    auto task_run = task_run_repository_.find_by_id(request.task_run_id);
    if (!task_run.has_value()) {
        return labbridge::core::Status::failure("task run is not found");
    }

    task_run->status = request.status;
    task_run->finished_at = request.finished_at;
    task_run->items_total = request.items_total;
    task_run->items_success = request.items_success;
    task_run->items_failed = request.items_failed;
    task_run->error_summary = request.error_summary;
    task_run_repository_.finish(*task_run);
    return labbridge::core::Status::success();
}

std::optional<TaskRunRecord> TaskRunService::find_run(const std::string& task_run_id) const {
    if (task_run_id.empty()) {
        return std::nullopt;
    }
    return task_run_repository_.find_by_id(task_run_id);
}

}  // namespace labbridge::server
