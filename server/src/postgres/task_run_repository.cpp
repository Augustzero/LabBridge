#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <stdexcept>
#include <utility>

namespace labbridge::server {
namespace {

TaskRunRecord to_task_run_record(const SqlRow& row) {
    TaskRunRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.task_id = storage::value_or_empty(row, "task_id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.status =
        storage::task_run_status_from_storage(storage::value_or_empty(row, "status"));
    record.started_at = storage::value_or_empty(row, "started_at");
    record.finished_at = storage::value_or_empty(row, "finished_at");
    record.items_total = storage::int_or_zero(row, "items_total");
    record.items_success = storage::int_or_zero(row, "items_success");
    record.items_failed = storage::int_or_zero(row, "items_failed");
    record.error_summary = storage::value_or_empty(row, "error_summary");
    record.trigger_type = storage::value_or_empty(row, "trigger_type");
    record.execution_key = storage::value_or_empty(row, "execution_key");
    record.scheduled_for = storage::value_or_empty(row, "scheduled_for");
    record.requested_at = storage::value_or_empty(row, "requested_at");
    record.retry_of_run_id = storage::value_or_empty(row, "retry_of_run_id");
    return record;
}

// 运行完整行投影；新字段只在这里补一处，各查询共用。
std::string task_run_select(const std::string& condition) {
    return "SELECT tr.id::text AS id, tr.task_id::text AS task_id, n.node_code, "
           "tr.status, " +
           storage::utc_column("tr.started_at", "started_at") + ", " +
           storage::utc_column("tr.finished_at", "finished_at") + ", "
           "tr.items_total::text AS items_total, tr.items_success::text AS items_success, "
           "tr.items_failed::text AS items_failed, COALESCE(tr.error_summary, '') AS error_summary, "
           "tr.trigger_type, COALESCE(tr.execution_key, '') AS execution_key, " +
           storage::utc_column("tr.scheduled_for", "scheduled_for") + ", " +
           storage::utc_column("tr.requested_at", "requested_at") + ", " +
           "COALESCE(tr.retry_of_run_id::text, '') AS retry_of_run_id "
           "FROM task_runs tr "
           "JOIN nodes n ON n.id = tr.node_id " +
           condition;
}

}  // namespace

PostgresTaskRunRepository::PostgresTaskRunRepository(ISqlSession& session) : session_(session) {}

std::string PostgresTaskRunRepository::create(TaskRunRecord task_run) {
    static const std::string sql =
        "INSERT INTO task_runs "
        "(task_id, node_id, status, started_at, items_total, items_success, items_failed, "
        "error_summary, trigger_type) "
        "SELECT t.id, n.id, $3, NULLIF($4, '')::timestamptz, $5::integer, $6::integer, "
        "$7::integer, NULLIF($8, ''), $9 "
        "FROM tasks t "
        "JOIN nodes n ON n.id = t.node_id "
        "WHERE t.id = $1::bigint AND n.node_code = $2 "
        "RETURNING id::text AS id";

    const auto row = session_.query_one(sql,
                                        {
                                            task_run.task_id,
                                            task_run.node_code,
                                            storage::to_storage(task_run.status),
                                            task_run.started_at,
                                            std::to_string(task_run.items_total),
                                            std::to_string(task_run.items_success),
                                            std::to_string(task_run.items_failed),
                                            task_run.error_summary,
                                            task_run.trigger_type,
                                        });
    if (!row.has_value()) {
        throw std::runtime_error("failed to create task run");
    }
    return storage::value_or_empty(*row, "id");
}

ScheduledTaskRunStart PostgresTaskRunRepository::create_or_find_scheduled(
    TaskRunRecord task_run) {
    static const std::string insert_sql =
        "INSERT INTO task_runs "
        "(task_id, node_id, status, started_at, items_total, items_success, items_failed, "
        "error_summary, trigger_type, execution_key, scheduled_for) "
        "SELECT t.id, n.id, $3, $4::timestamptz, $5::integer, $6::integer, "
        "$7::integer, NULLIF($8, ''), $9, $10, $11::timestamptz "
        "FROM tasks t "
        "JOIN nodes n ON n.id = t.node_id "
        "WHERE t.id = $1::bigint AND n.node_code = $2 "
        "ON CONFLICT (node_id, execution_key) WHERE execution_key IS NOT NULL "
        "DO NOTHING "
        "RETURNING id::text AS id";

    const SqlParams params{
        task_run.task_id,
        task_run.node_code,
        storage::to_storage(task_run.status),
        task_run.started_at,
        std::to_string(task_run.items_total),
        std::to_string(task_run.items_success),
        std::to_string(task_run.items_failed),
        task_run.error_summary,
        task_run.trigger_type,
        task_run.execution_key,
        task_run.scheduled_for,
    };
    const auto inserted = session_.query_one(insert_sql, params);
    if (inserted.has_value()) {
        task_run.id = storage::value_or_empty(*inserted, "id");
        return {std::move(task_run), true};
    }

    // INSERT 在并发冲突时会等待首次事务结束；下一条语句的新快照可读取其结果。
    static const std::string existing_sql =
        task_run_select("WHERE n.node_code = $1 AND tr.execution_key = $2 "
                        "LIMIT 1");
    const auto existing = session_.query_one(
        existing_sql, {task_run.node_code, task_run.execution_key});
    if (!existing.has_value()) {
        throw std::runtime_error("failed to create or find scheduled task run");
    }
    return {to_task_run_record(*existing), false};
}

std::optional<ManualTaskRunStart>
PostgresTaskRunRepository::create_or_find_manual(
    ManualTaskRunRequest request) {
    // 只有 retry 运行才有固定输入快照；manual / 空 retry 保持 NULL，
    // 报告校验据此区分“固定清单运行”和普通运行。
    const auto retry_files_json = request.retry_files.empty()
        ? std::string{}
        : to_retry_files_json(request.retry_files);
    static const std::string insert_sql =
        "INSERT INTO task_runs "
        "(task_id, node_id, status, trigger_type, execution_key, requested_at, "
        "retry_of_run_id, retry_files) "
        "SELECT t.id, n.id, 'pending', $3, $4, now(), "
        "NULLIF($5, '')::bigint, NULLIF($6, '')::jsonb "
        "FROM tasks t "
        "JOIN nodes n ON n.id = t.node_id "
        "WHERE t.id = $1::bigint AND n.node_code = $2 "
        "ON CONFLICT DO NOTHING "
        "RETURNING id::text AS id";

    const auto inserted = session_.query_one(
        insert_sql,
        {request.task_id,
         request.node_code,
         request.trigger_type,
         request.execution_key,
         request.retry_of_run_id,
         retry_files_json});
    if (inserted.has_value()) {
        // 新受理的请求先重读完整行，requested_at 等库端时间以读取为准。
        const auto created =
            find_by_id(storage::value_or_empty(*inserted, "id"));
        if (!created.has_value()) {
            throw std::runtime_error("created manual task run is not readable");
        }
        return ManualTaskRunStart{*created, true};
    }

    // 冲突可能来自执行键重放，也可能来自同任务的 pending 唯一索引；
    // 这里只按执行键回读，查不到就让调用方去查 pending 冲突。
    const auto existing = find_by_execution_key(request.execution_key);
    if (!existing.has_value()) {
        return std::nullopt;
    }
    return ManualTaskRunStart{*existing, false};
}

std::optional<TaskRunRecord> PostgresTaskRunRepository::find_by_id(
    const std::string& task_run_id) const {
    static const std::string sql =
        task_run_select("WHERE tr.id = $1::bigint LIMIT 1");
    const auto row = session_.query_one(sql, {task_run_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_task_run_record(*row);
}

void PostgresTaskRunRepository::finish(TaskRunRecord task_run) {
    static const std::string sql =
        "UPDATE task_runs SET "
        "status = $2, "
        "finished_at = NULLIF($3, '')::timestamptz, "
        "items_total = $4::integer, "
        "items_success = $5::integer, "
        "items_failed = $6::integer, "
        "error_summary = NULLIF($7, ''), "
        "failed_files = NULLIF($8, '')::jsonb "
        "WHERE id = $1::bigint";

    session_.execute(sql,
                     {
                         task_run.id,
                         storage::to_storage(task_run.status),
                         task_run.finished_at,
                         std::to_string(task_run.items_total),
                         std::to_string(task_run.items_success),
                         std::to_string(task_run.items_failed),
                         task_run.error_summary,
                         task_run.failed_files_json,
                     });
}

std::optional<TaskRunRecord> PostgresTaskRunRepository::find_by_execution_key(
    const std::string& execution_key) const {
    static const std::string sql =
        task_run_select("WHERE tr.execution_key = $1 "
                        "LIMIT 1");
    const auto row = session_.query_one(sql, {execution_key});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_task_run_record(*row);
}

std::optional<std::vector<TaskRunFailedFile>>
PostgresTaskRunRepository::find_failed_files(
    const std::string& task_run_id) const {
    static const std::string sql =
        "SELECT COALESCE(failed_files::text, '') AS failed_files "
        "FROM task_runs WHERE id = $1::bigint";
    const auto row = session_.query_one(sql, {task_run_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return parse_failed_files_json(
        storage::value_or_empty(*row, "failed_files"));
}

std::optional<std::vector<TaskRunRetryFile>>
PostgresTaskRunRepository::find_retry_files(
    const std::string& task_run_id) const {
    static const std::string sql =
        "SELECT COALESCE(retry_files::text, '') AS retry_files "
        "FROM task_runs WHERE id = $1::bigint";
    const auto row = session_.query_one(sql, {task_run_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return parse_retry_files_json(
        storage::value_or_empty(*row, "retry_files"));
}

bool PostgresTaskRunRepository::lock_task(const std::string& task_id) {
    // 只锁任务行本身；调用方按“任务行 → 运行行”的固定顺序拿锁防死锁。
    static const std::string sql =
        "SELECT 1 FROM tasks t WHERE t.id = $1::bigint FOR UPDATE";
    return session_.query_one(sql, {task_id}).has_value();
}

std::optional<TaskRunRecord> PostgresTaskRunRepository::lock_by_id(
    const std::string& task_run_id) {
    static const std::string sql =
        task_run_select("WHERE tr.id = $1::bigint LIMIT 1 ") +
        "FOR UPDATE OF tr";
    const auto row = session_.query_one(sql, {task_run_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_task_run_record(*row);
}

std::optional<TaskRunRecord>
PostgresTaskRunRepository::find_pending_manual_by_task(
    const std::string& task_id) const {
    static const std::string sql = task_run_select(
        "WHERE tr.task_id = $1::bigint AND tr.status = 'pending' "
        "AND tr.trigger_type IN ('manual', 'retry') "
        "ORDER BY tr.id LIMIT 1");
    const auto row = session_.query_one(sql, {task_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_task_run_record(*row);
}

void PostgresTaskRunRepository::fail_pending_manual_runs(
    const std::string& task_id, const std::string& error_summary) {
    // 空数组表示“明确没有文件级失败”，与旧报告的 NULL 区分开。
    static const std::string sql =
        "UPDATE task_runs SET "
        "status = 'failed', "
        "finished_at = now(), "
        "error_summary = $2, "
        "failed_files = '[]'::jsonb "
        "WHERE task_id = $1::bigint AND status = 'pending' "
        "AND trigger_type IN ('manual', 'retry')";
    session_.execute(sql, {task_id, error_summary});
}

void PostgresTaskRunRepository::mark_manual_running(
    const std::string& task_run_id, const std::string& started_at) {
    static const std::string sql =
        "UPDATE task_runs SET status = 'running', started_at = $2::timestamptz "
        "WHERE id = $1::bigint AND status = 'pending'";
    session_.execute(sql, {task_run_id, started_at});
}

std::vector<PendingExecutionRecord>
PostgresTaskRunRepository::find_pending_executions_by_node(
    const std::string& node_code) const {
    static const std::string sql =
        "SELECT tr.id::text AS task_run_id, tr.task_id::text AS task_id, "
        "tr.execution_key, tr.trigger_type, " +
        storage::utc_column("tr.requested_at", "requested_at") + ", "
        "COALESCE(tr.retry_files::text, '') AS retry_files "
        "FROM task_runs tr "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE n.node_code = $1 AND tr.status = 'pending' "
        "AND tr.trigger_type IN ('manual', 'retry') "
        "ORDER BY tr.id";
    std::vector<PendingExecutionRecord> executions;
    for (const auto& row : session_.query_all(sql, {node_code})) {
        PendingExecutionRecord execution;
        execution.task_run_id = storage::value_or_empty(row, "task_run_id");
        execution.task_id = storage::value_or_empty(row, "task_id");
        execution.execution_key = storage::value_or_empty(row, "execution_key");
        execution.trigger_type = storage::value_or_empty(row, "trigger_type");
        execution.requested_at = storage::value_or_empty(row, "requested_at");
        auto retry_files = parse_retry_files_json(
            storage::value_or_empty(row, "retry_files"));
        if (retry_files.has_value()) {
            execution.retry_files = std::move(*retry_files);
        }
        executions.push_back(std::move(execution));
    }
    return executions;
}

}  // namespace labbridge::server
