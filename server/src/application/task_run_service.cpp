#include "labbridge/server/application/task_run_service.h"

#include <cctype>
#include <string_view>
#include <utility>

namespace labbridge::server {
namespace {

int decimal_component(std::string_view value, std::size_t offset, std::size_t length) {
    int result = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return -1;
        }
        result = (result * 10) + (value[index] - '0');
    }
    return result;
}

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool is_rfc3339_utc(std::string_view value) {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') {
        return false;
    }

    const int year = decimal_component(value, 0, 4);
    const int month = decimal_component(value, 5, 2);
    const int day = decimal_component(value, 8, 2);
    const int hour = decimal_component(value, 11, 2);
    const int minute = decimal_component(value, 14, 2);
    const int second = decimal_component(value, 17, 2);
    if (year < 1 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    constexpr int kDaysByMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maximum_day = kDaysByMonth[month - 1];
    if (month == 2 && is_leap_year(year)) {
        maximum_day = 29;
    }
    return day >= 1 && day <= maximum_day;
}

}  // namespace

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
    const bool scheduled_start =
        !request.execution_key.empty() || !request.scheduled_for.empty();
    if (scheduled_start) {
        if (request.execution_key.empty()) {
            return {labbridge::core::Status::failure("execution_key is required"), {}};
        }
        if (request.execution_key.size() > 128) {
            return {labbridge::core::Status::failure(
                        "execution_key must not exceed 128 characters"),
                    {}};
        }
        if (request.trigger_type != "scheduled") {
            return {labbridge::core::Status::failure(
                        "trigger_type must be scheduled"),
                    {}};
        }
        if (!is_rfc3339_utc(request.scheduled_for)) {
            return {labbridge::core::Status::failure(
                        "scheduled_for must be an RFC 3339 UTC timestamp"),
                    {}};
        }
        if (!is_rfc3339_utc(request.started_at)) {
            return {labbridge::core::Status::failure(
                        "started_at must be an RFC 3339 UTC timestamp"),
                    {}};
        }
    }

    const auto task = config_repository_.find_task(request.task_id);
    if (!task.has_value()) {
        return {labbridge::core::Status::failure(labbridge::core::StatusCode::NotFound, "task is not found"), {}};
    }
    if (!task->enabled) {
        return {labbridge::core::Status::failure(labbridge::core::StatusCode::Conflict, "task is disabled"), {}};
    }
    if (task->node_code != request.node_code) {
        return {labbridge::core::Status::failure(
                    labbridge::core::StatusCode::Conflict,
                    "task does not belong to node"),
                {}};
    }

    TaskRunRecord record;
    record.task_id = request.task_id;
    record.node_code = request.node_code;
    record.status = labbridge::core::TaskRunStatus::Running;
    record.started_at = request.started_at;
    record.trigger_type = request.trigger_type.empty() ? "scheduled" : request.trigger_type;
    record.execution_key = request.execution_key;
    record.scheduled_for = request.scheduled_for;

    if (!scheduled_start) {
        const auto id = task_run_repository_.create(std::move(record));
        return {labbridge::core::Status::success(), id, false};
    }

    // 数据库唯一约束决定首次创建者；服务层只比较稳定的计划身份。
    const auto started = task_run_repository_.create_or_find_scheduled(
        std::move(record));
    if (!started.created &&
        (started.task_run.task_id != request.task_id ||
         started.task_run.scheduled_for != request.scheduled_for)) {
        return {
            labbridge::core::Status::failure(
                labbridge::core::StatusCode::Conflict,
                "execution_key was already used for a different scheduled run"),
            {},
            false,
        };
    }
    return {
        labbridge::core::Status::success(),
        started.task_run.id,
        !started.created,
    };
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
        return labbridge::core::Status::failure(labbridge::core::StatusCode::NotFound, "task run is not found");
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
