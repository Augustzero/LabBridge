#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <stdexcept>

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
    return record;
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

std::optional<TaskRunRecord> PostgresTaskRunRepository::find_by_id(
    const std::string& task_run_id) const {
    static const std::string sql =
        "SELECT tr.id::text AS id, tr.task_id::text AS task_id, n.node_code, tr.status, "
        "COALESCE(to_char(tr.started_at, 'YYYY-MM-DD HH24:MI:SS'), '') AS started_at, "
        "COALESCE(to_char(tr.finished_at, 'YYYY-MM-DD HH24:MI:SS'), '') AS finished_at, "
        "tr.items_total::text AS items_total, tr.items_success::text AS items_success, "
        "tr.items_failed::text AS items_failed, COALESCE(tr.error_summary, '') AS error_summary, "
        "tr.trigger_type "
        "FROM task_runs tr "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE tr.id = $1::bigint "
        "LIMIT 1";

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
        "error_summary = NULLIF($7, '') "
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
                     });
}

}  // namespace labbridge::server
