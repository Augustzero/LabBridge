#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <stdexcept>

namespace labbridge::server {
namespace {

DataSourceRecord to_data_source_record(const SqlRow& row) {
    DataSourceRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.source_type =
        storage::source_type_from_storage(storage::value_or_empty(row, "source_type"));
    record.name = storage::value_or_empty(row, "name");
    record.config_json = storage::value_or_empty(row, "config_json");
    record.enabled = storage::bool_value(storage::value_or_empty(row, "enabled"));
    return record;
}

TaskRecord to_task_record(const SqlRow& row) {
    TaskRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.data_source_id = storage::value_or_empty(row, "data_source_id");
    record.name = storage::value_or_empty(row, "name");
    record.task_type = storage::value_or_empty(row, "task_type");
    record.schedule_expr = storage::value_or_empty(row, "schedule_expr");
    record.parser_type = storage::value_or_empty(row, "parser_type");
    record.qc_profile = storage::value_or_empty(row, "qc_profile");
    record.enabled = storage::bool_value(storage::value_or_empty(row, "enabled"));
    return record;
}

}  // namespace

PostgresConfigRepository::PostgresConfigRepository(ISqlSession& session) : session_(session) {}

std::string PostgresConfigRepository::create_data_source(DataSourceRecord data_source) {
    static const std::string sql =
        "INSERT INTO data_sources (node_id, source_type, name, config_json, enabled) "
        "SELECT id, $2, $3, $4::jsonb, $5::boolean "
        "FROM nodes WHERE node_code = $1 "
        "RETURNING id::text AS id";

    const auto row = session_.query_one(sql,
                                        {
                                            data_source.node_code,
                                            storage::to_storage(data_source.source_type),
                                            data_source.name,
                                            data_source.config_json,
                                            storage::bool_param(data_source.enabled),
                                        });
    if (!row.has_value()) {
        throw std::runtime_error("failed to create data source");
    }
    return storage::value_or_empty(*row, "id");
}

std::optional<DataSourceRecord> PostgresConfigRepository::find_data_source(
    const std::string& data_source_id) const {
    static const std::string sql =
        "SELECT ds.id::text AS id, n.node_code, ds.source_type, ds.name, "
        "ds.config_json::text AS config_json, "
        "CASE WHEN ds.enabled THEN 'true' ELSE 'false' END AS enabled "
        "FROM data_sources ds "
        "JOIN nodes n ON n.id = ds.node_id "
        "WHERE ds.id = $1::bigint "
        "LIMIT 1";

    const auto row = session_.query_one(sql, {data_source_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_data_source_record(*row);
}

std::string PostgresConfigRepository::create_task(TaskRecord task) {
    static const std::string sql =
        "INSERT INTO tasks "
        "(node_id, data_source_id, name, task_type, schedule_expr, parser_type, qc_profile, enabled) "
        "SELECT n.id, ds.id, $3, $4, $5, $6, NULLIF($7, ''), $8::boolean "
        "FROM nodes n "
        "JOIN data_sources ds ON ds.id = $2::bigint AND ds.node_id = n.id "
        "WHERE n.node_code = $1 "
        "RETURNING id::text AS id";

    const auto row = session_.query_one(sql,
                                        {
                                            task.node_code,
                                            task.data_source_id,
                                            task.name,
                                            task.task_type,
                                            task.schedule_expr,
                                            task.parser_type,
                                            task.qc_profile,
                                            storage::bool_param(task.enabled),
                                        });
    if (!row.has_value()) {
        throw std::runtime_error("failed to create task");
    }
    return storage::value_or_empty(*row, "id");
}

std::optional<TaskRecord> PostgresConfigRepository::find_task(const std::string& task_id) const {
    static const std::string sql =
        "SELECT t.id::text AS id, n.node_code, t.data_source_id::text AS data_source_id, "
        "t.name, t.task_type, t.schedule_expr, t.parser_type, "
        "COALESCE(t.qc_profile, '') AS qc_profile, "
        "CASE WHEN t.enabled THEN 'true' ELSE 'false' END AS enabled "
        "FROM tasks t "
        "JOIN nodes n ON n.id = t.node_id "
        "WHERE t.id = $1::bigint "
        "LIMIT 1";

    const auto row = session_.query_one(sql, {task_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_task_record(*row);
}

std::vector<TaskRecord> PostgresConfigRepository::find_enabled_tasks_by_node(
    const std::string& node_code) const {
    static const std::string sql =
        "SELECT t.id::text AS id, n.node_code, t.data_source_id::text AS data_source_id, "
        "t.name, t.task_type, t.schedule_expr, t.parser_type, "
        "COALESCE(t.qc_profile, '') AS qc_profile, "
        "CASE WHEN t.enabled THEN 'true' ELSE 'false' END AS enabled "
        "FROM tasks t "
        "JOIN nodes n ON n.id = t.node_id "
        "WHERE n.node_code = $1 AND t.enabled = true "
        "ORDER BY t.id";

    std::vector<TaskRecord> tasks;
    for (const auto& row : session_.query_all(sql, {node_code})) {
        tasks.push_back(to_task_record(row));
    }
    return tasks;
}

}  // namespace labbridge::server
