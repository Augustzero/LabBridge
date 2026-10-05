#include "labbridge/server/application/task_run_service.h"

#include "labbridge/server/application/id_validation.h"
#include "labbridge/server/application/task_executability.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
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

bool is_hexadecimal(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
        return std::isdigit(ch) != 0 ||
               (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    });
}

// 前端一次按钮操作生成一个 32 位随机十六进制键，同一网络重试复用。
bool is_operator_idempotency_key(const std::string& key) {
    return key.size() == 32 && is_hexadecimal(key);
}

labbridge::core::Status invalid(std::string message) {
    return labbridge::core::Status::failure(
        labbridge::core::StatusCode::InvalidArgument, std::move(message));
}

labbridge::core::Status not_found(std::string message) {
    return labbridge::core::Status::failure(
        labbridge::core::StatusCode::NotFound, std::move(message));
}

labbridge::core::Status conflict(std::string message) {
    return labbridge::core::Status::failure(
        labbridge::core::StatusCode::Conflict, std::move(message));
}

}  // namespace

TaskRunService::TaskRunService(IConfigRepository& config_repository,
                               ITaskRunRepository& task_run_repository,
                               INodeRepository& node_repository,
                               IQcRepository& qc_repository,
                               IResultRepository& result_repository)
    : config_repository_(config_repository),
      task_run_repository_(task_run_repository),
      node_repository_(node_repository),
      qc_repository_(qc_repository),
      result_repository_(result_repository) {}

TaskRunCreateResult TaskRunService::start(const StartTaskRunRequest& request) {
    if (request.node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"), {}};
    }
    if (request.task_id.empty()) {
        return {labbridge::core::Status::failure("task_id is required"), {}};
    }
    if (request.trigger_type == "manual" || request.trigger_type == "retry") {
        return start_manual(request);
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
        return {not_found("task is not found"), {}};
    }
    if (!task->enabled) {
        return {conflict("task is disabled"), {}};
    }
    if (task->node_code != request.node_code) {
        return {conflict("task does not belong to node"), {}};
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
        return {labbridge::core::Status::success(), id, false, "running"};
    }

    // 数据库唯一约束决定首次创建者；服务层只比较稳定的计划身份。
    const auto started = task_run_repository_.create_or_find_scheduled(
        std::move(record));
    if (!started.created &&
        (started.task_run.task_id != request.task_id ||
         started.task_run.scheduled_for != request.scheduled_for)) {
        return {
            conflict(
                "execution_key was already used for a different scheduled run"),
            {},
            false,
            {},
        };
    }
    return {
        labbridge::core::Status::success(),
        started.task_run.id,
        !started.created,
        "running",
    };
}

TaskRunCreateResult TaskRunService::start_manual(
    const StartTaskRunRequest& request) {
    if (request.execution_key.empty() ||
        request.execution_key.size() > 128) {
        return {invalid("execution_key is required"), {}};
    }
    if (!request.scheduled_for.empty()) {
        return {invalid("scheduled_for must be empty for manual task runs"), {}};
    }
    if (!is_rfc3339_utc(request.started_at)) {
        return {invalid("started_at must be an RFC 3339 UTC timestamp"), {}};
    }

    // 人工运行只能来自中心受理过的 pending 请求，Agent 不能凭空创建。
    auto run = task_run_repository_.find_by_execution_key(
        request.execution_key);
    if (!run.has_value() || run->node_code != request.node_code ||
        run->task_id != request.task_id ||
        run->trigger_type != request.trigger_type) {
        return {conflict(
                    "execution_key does not match an accepted manual task run"),
                {},
                false,
                {}};
    }

    // 原作业重放先于启用状态检查：已 running / 终态的运行直接返回现状，
    // 不因任务随后被禁用而无法恢复进度。
    if (run->status != labbridge::core::TaskRunStatus::Pending) {
        return replay_result(*run);
    }

    // “任务行 → 运行行”加锁：与任务禁用按同序拿锁，竞争只有一种结果。
    if (!task_run_repository_.lock_task(run->task_id)) {
        return {not_found("task is not found"), {}};
    }
    const auto locked = task_run_repository_.lock_by_id(run->id);
    if (!locked.has_value()) {
        return {not_found("task run is not found"), {}};
    }
    // 拿到运行行锁后若已不是 pending，说明禁用事务刚刚把它收尾了。
    if (locked->status != labbridge::core::TaskRunStatus::Pending) {
        return replay_result(*locked);
    }
    task_run_repository_.mark_manual_running(run->id, request.started_at);
    return {labbridge::core::Status::success(), run->id, false, "running"};
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
        return not_found("task run is not found");
    }

    task_run->status = request.status;
    task_run->finished_at = request.finished_at;
    task_run->items_total = request.items_total;
    task_run->items_success = request.items_success;
    task_run->items_failed = request.items_failed;
    task_run->error_summary = request.error_summary;
    task_run->failed_files_json = request.has_failed_files
        ? to_failed_files_json(request.failed_files)
        : "";
    task_run_repository_.finish(*task_run);
    return labbridge::core::Status::success();
}

std::optional<TaskRunRecord> TaskRunService::find_run(const std::string& task_run_id) const {
    if (task_run_id.empty()) {
        return std::nullopt;
    }
    return task_run_repository_.find_by_id(task_run_id);
}

std::optional<std::vector<TaskRunRetryFile>>
TaskRunService::find_retry_files(const std::string& task_run_id) const {
    if (task_run_id.empty()) {
        return std::nullopt;
    }
    return task_run_repository_.find_retry_files(task_run_id);
}

ManualTaskRunResult TaskRunService::request_manual_run(
    const std::string& task_id,
    const std::string& idempotency_key) {
    return apply_manual_run("manual", task_id, "", idempotency_key, {});
}

ManualTaskRunResult TaskRunService::request_retry_run(
    const std::string& task_run_id,
    const std::string& idempotency_key) {
    if (!is_operator_idempotency_key(idempotency_key)) {
        return {invalid("idempotency_key must be 32 hexadecimal characters"),
                {},
                {},
                false};
    }
    const auto execution_key = "operator:" + idempotency_key;

    // 重放判定先于一切新请求条件：已受理的操作不能因任务后来禁用而无法确认。
    const auto existing =
        task_run_repository_.find_by_execution_key(execution_key);
    if (existing.has_value()) {
        if (existing->trigger_type != "retry" ||
            existing->retry_of_run_id != task_run_id) {
            return {conflict(
                        "idempotency_key was already used for a different request"),
                    {},
                    {},
                    false};
        }
        return replayed_result(*existing);
    }

    if (!is_positive_id(task_run_id)) {
        return {invalid("task_run_id must be a positive integer"), {}, {}, false};
    }
    const auto parent_run = task_run_repository_.find_by_id(task_run_id);
    if (!parent_run.has_value()) {
        return {not_found("task run is not found"), {}, {}, false};
    }
    if (parent_run->status != labbridge::core::TaskRunStatus::Failed) {
        return {conflict("only failed task runs can be retried"), {}, {}, false};
    }
    const auto failed_files =
        task_run_repository_.find_failed_files(task_run_id);
    if (!failed_files.has_value()) {
        return {conflict(
                    "task run has no failed file list and cannot be retried"),
                {},
                {},
                false};
    }
    if (failed_files->empty()) {
        return {conflict("task run has no failed files to retry"), {}, {}, false};
    }

    std::vector<TaskRunRetryFile> retry_files;
    const auto resolve_status =
        resolve_retry_files(*parent_run, *failed_files, retry_files);
    if (!resolve_status.ok) {
        return {resolve_status, {}, {}, false};
    }

    return apply_manual_run("retry", parent_run->task_id, parent_run->id,
                            idempotency_key, std::move(retry_files));
}

ManualTaskRunResult TaskRunService::apply_manual_run(
    const std::string& trigger_type,
    const std::string& task_id,
    const std::string& retry_of_run_id,
    const std::string& idempotency_key,
    std::vector<TaskRunRetryFile> retry_files) {
    if (!is_operator_idempotency_key(idempotency_key)) {
        return {invalid("idempotency_key must be 32 hexadecimal characters"),
                {},
                {},
                false};
    }
    const auto execution_key = "operator:" + idempotency_key;

    // 重放判定先于任务校验，与 request_retry_run 同一口径。
    const auto existing =
        task_run_repository_.find_by_execution_key(execution_key);
    if (existing.has_value()) {
        if (existing->trigger_type != trigger_type ||
            existing->task_id != task_id ||
            existing->retry_of_run_id != retry_of_run_id) {
            return {conflict(
                        "idempotency_key was already used for a different request"),
                    {},
                    {},
                    false};
        }
        return replayed_result(*existing);
    }

    if (!is_positive_id(task_id)) {
        return {invalid("task_id must be a positive integer"), {}, {}, false};
    }
    if (!task_run_repository_.lock_task(task_id)) {
        return {not_found("task is not found"), {}, {}, false};
    }
    const auto task = config_repository_.find_task(task_id);
    if (!task.has_value()) {
        return {not_found("task is not found"), {}, {}, false};
    }
    if (!task->enabled) {
        return {conflict("task is disabled"), {}, {}, false};
    }
    const auto dependency_status = validate_task_executable(
        node_repository_, config_repository_, qc_repository_, *task,
        config_repository_.find_task_qc_rule_ids(task_id));
    if (!dependency_status.ok) {
        return {dependency_status, {}, {}, false};
    }

    const auto pending =
        task_run_repository_.find_pending_manual_by_task(task_id);
    if (pending.has_value() && pending->execution_key != execution_key) {
        return {conflict("task already has a pending manual run: " +
                         pending->id),
                {},
                {},
                false};
    }

    const auto started = task_run_repository_.create_or_find_manual(
        {task_id,
         task->node_code,
         trigger_type,
         execution_key,
         retry_of_run_id,
         std::move(retry_files)});
    if (!started.has_value()) {
        // 执行键没查到又没插进去：只可能是并发请求抢先占了同任务的 pending 槽。
        const auto raced =
            task_run_repository_.find_pending_manual_by_task(task_id);
        if (raced.has_value()) {
            return {conflict("task already has a pending manual run: " +
                             raced->id),
                    {},
                    {},
                    false};
        }
        throw std::runtime_error("failed to create manual task run");
    }
    return {
        labbridge::core::Status::success(),
        started->task_run.id,
        storage::to_storage(started->task_run.status),
        !started->created,
    };
}

labbridge::core::Status TaskRunService::resolve_retry_files(
    const TaskRunRecord& parent_run,
    const std::vector<TaskRunFailedFile>& failed_files,
    std::vector<TaskRunRetryFile>& retry_files) const {
    // 只核对数据库事实，不访问 Agent 文件系统；
    // 文件现在是否存在、可读，由 Agent 执行时判断。
    for (const auto& failed : failed_files) {
        if (failed.source_path.empty() || failed.original_name.empty()) {
            return conflict(
                "failed file entry does not identify a concrete target");
        }

        TaskRunRetryFile retry_file;
        retry_file.source_path = failed.source_path;
        retry_file.original_name = failed.original_name;
        if (failed.archive_raw_file_id.empty()) {
            // 从未取得有效归档：定点补采，内容在 Agent 读取时固定。
            retry_file.input_type = "source";
            retry_files.push_back(std::move(retry_file));
            continue;
        }

        const auto raw_file =
            result_repository_.find_raw_file(failed.archive_raw_file_id);
        if (!raw_file.has_value()) {
            return conflict("archived input is not available: " +
                            failed.archive_raw_file_id);
        }
        if (raw_file->node_code != parent_run.node_code) {
            return conflict("archived input does not belong to the node");
        }
        const auto raw_run =
            task_run_repository_.find_by_id(raw_file->task_run_id);
        if (!raw_run.has_value() ||
            raw_run->task_id != parent_run.task_id) {
            return conflict("archived input does not belong to the task");
        }
        if (raw_file->storage_path.empty() || raw_file->file_hash.empty()) {
            // 缺必要归档元数据只能拒绝，不能降级为定点补采。
            return conflict("archived input is missing storage metadata");
        }

        retry_file.input_type = "archive";
        retry_file.archive_raw_file_id = raw_file->id;
        retry_file.storage_path = raw_file->storage_path;
        retry_file.size_bytes = raw_file->size_bytes;
        retry_file.file_hash = raw_file->file_hash;
        retry_file.source_mtime = raw_file->source_mtime;
        retry_files.push_back(std::move(retry_file));
    }
    return labbridge::core::Status::success();
}

ManualTaskRunResult TaskRunService::replayed_result(
    const TaskRunRecord& run) const {
    return {
        labbridge::core::Status::success(),
        run.id,
        storage::to_storage(run.status),
        true,
    };
}

TaskRunCreateResult TaskRunService::replay_result(const TaskRunRecord& run) const {
    return {
        labbridge::core::Status::success(),
        run.id,
        true,
        storage::to_storage(run.status),
    };
}

}  // namespace labbridge::server
