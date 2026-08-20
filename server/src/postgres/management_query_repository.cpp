#include "labbridge/server/postgres/management_query_repository.h"

#include "labbridge/server/postgres/storage_mapping.h"

#include <sstream>
#include <utility>

namespace labbridge::server {
namespace {

std::string bind(SqlParams& params, std::string value) {
    params.push_back(std::move(value));
    return "$" + std::to_string(params.size());
}

void add_cursor(
    std::string& sql,
    SqlParams& params,
    const std::string& qualified_id,
    const ManagementPageRequest& page) {
    if (page.cursor.has_value()) {
        sql += " AND " + qualified_id + " < " +
               bind(params, *page.cursor) + "::bigint";
    }
}

void add_limit(
    std::string& sql,
    SqlParams& params,
    const ManagementPageRequest& page) {
    sql += " ORDER BY id DESC LIMIT " +
           bind(params, std::to_string(page.fetch_limit)) + "::integer";
}

std::string utc_column(
    const std::string& expression,
    const std::string& alias) {
    return "COALESCE(to_char(" + expression +
           " AT TIME ZONE 'UTC', "
           "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') AS " + alias;
}

NodeRecord to_node(const SqlRow& row) {
    NodeRecord node;
    node.id = storage::value_or_empty(row, "id");
    node.info.node_code = storage::value_or_empty(row, "node_code");
    node.info.name = storage::value_or_empty(row, "name");
    node.info.agent_version =
        storage::value_or_empty(row, "agent_version");
    node.status = storage::node_status_from_storage(
        storage::value_or_empty(row, "status"));
    node.last_heartbeat_at =
        storage::value_or_empty(row, "last_heartbeat_at");
    node.created_at = storage::value_or_empty(row, "created_at");
    node.updated_at = storage::value_or_empty(row, "updated_at");
    return node;
}

DataSourceRecord to_data_source(const SqlRow& row) {
    DataSourceRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.source_type = storage::source_type_from_storage(
        storage::value_or_empty(row, "source_type"));
    record.name = storage::value_or_empty(row, "name");
    record.config_json = storage::value_or_empty(row, "config_json");
    record.enabled = storage::bool_value(
        storage::value_or_empty(row, "enabled"));
    record.created_at = storage::value_or_empty(row, "created_at");
    record.updated_at = storage::value_or_empty(row, "updated_at");
    return record;
}

std::vector<std::string> split_ids(const std::string& value) {
    std::vector<std::string> ids;
    std::istringstream input{value};
    std::string id;
    while (std::getline(input, id, ',')) {
        if (!id.empty()) {
            ids.push_back(std::move(id));
        }
    }
    return ids;
}

TaskRecord to_task(const SqlRow& row) {
    TaskRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.data_source_id =
        storage::value_or_empty(row, "data_source_id");
    record.name = storage::value_or_empty(row, "name");
    record.task_type = storage::value_or_empty(row, "task_type");
    record.schedule_expr =
        storage::value_or_empty(row, "schedule_expr");
    record.parser_type = storage::value_or_empty(row, "parser_type");
    record.qc_profile = storage::value_or_empty(row, "qc_profile");
    record.enabled = storage::bool_value(
        storage::value_or_empty(row, "enabled"));
    record.qc_rule_ids = split_ids(
        storage::value_or_empty(row, "qc_rule_ids"));
    record.created_at = storage::value_or_empty(row, "created_at");
    record.updated_at = storage::value_or_empty(row, "updated_at");
    return record;
}

QcRuleRecord to_qc_rule(const SqlRow& row) {
    QcRuleRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.name = storage::value_or_empty(row, "name");
    record.rule_type = storage::value_or_empty(row, "rule_type");
    record.rule_config_json =
        storage::value_or_empty(row, "rule_config_json");
    record.enabled = storage::bool_value(
        storage::value_or_empty(row, "enabled"));
    record.created_at = storage::value_or_empty(row, "created_at");
    return record;
}

TaskRunRecord to_task_run(const SqlRow& row) {
    TaskRunRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.task_id = storage::value_or_empty(row, "task_id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.status = storage::task_run_status_from_storage(
        storage::value_or_empty(row, "status"));
    record.started_at = storage::value_or_empty(row, "started_at");
    record.finished_at = storage::value_or_empty(row, "finished_at");
    record.items_total = storage::int_or_zero(row, "items_total");
    record.items_success = storage::int_or_zero(row, "items_success");
    record.items_failed = storage::int_or_zero(row, "items_failed");
    record.error_summary =
        storage::value_or_empty(row, "error_summary");
    record.trigger_type =
        storage::value_or_empty(row, "trigger_type");
    record.execution_key =
        storage::value_or_empty(row, "execution_key");
    record.scheduled_for =
        storage::value_or_empty(row, "scheduled_for");
    return record;
}

RawFileRecord to_raw_file(const SqlRow& row) {
    RawFileRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.task_run_id =
        storage::value_or_empty(row, "task_run_id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.original_name =
        storage::value_or_empty(row, "original_name");
    record.file_hash = storage::value_or_empty(row, "file_hash");
    record.storage_path =
        storage::value_or_empty(row, "storage_path");
    record.size_bytes = storage::int64_or_zero(row, "size_bytes");
    record.source_mtime =
        storage::value_or_empty(row, "source_mtime");
    record.ingest_status =
        storage::value_or_empty(row, "ingest_status");
    record.created_at = storage::value_or_empty(row, "created_at");
    return record;
}

ParsedRecordRecord to_parsed_record(const SqlRow& row) {
    ParsedRecordRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.raw_file_id =
        storage::value_or_empty(row, "raw_file_id");
    record.task_run_id =
        storage::value_or_empty(row, "task_run_id");
    record.record.station_code =
        storage::value_or_empty(row, "station_code");
    record.record.device_code =
        storage::value_or_empty(row, "device_code");
    record.record.record_time =
        storage::value_or_empty(row, "record_time");
    record.record.payload_json =
        storage::value_or_empty(row, "payload_json");
    record.parse_status =
        storage::value_or_empty(row, "parse_status");
    record.created_at = storage::value_or_empty(row, "created_at");
    return record;
}

QcResultRecord to_qc_result(const SqlRow& row) {
    QcResultRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.parsed_record_id =
        storage::value_or_empty(row, "parsed_record_id");
    record.qc_rule_id =
        storage::value_or_empty(row, "qc_rule_id");
    record.level = storage::value_or_empty(row, "level");
    record.result = storage::value_or_empty(row, "result");
    record.message = storage::value_or_empty(row, "message");
    record.task_run_id =
        storage::value_or_empty(row, "task_run_id");
    record.created_at = storage::value_or_empty(row, "created_at");
    return record;
}

AlertRecord to_alert(const SqlRow& row) {
    AlertRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.task_run_id =
        storage::value_or_empty(row, "task_run_id");
    record.alert_type =
        storage::value_or_empty(row, "alert_type");
    record.severity = storage::value_or_empty(row, "severity");
    record.message = storage::value_or_empty(row, "message");
    record.status = storage::value_or_empty(row, "status");
    record.created_at = storage::value_or_empty(row, "created_at");
    return record;
}

std::string task_columns() {
    return
        "t.id::text AS id, n.node_code, "
        "t.data_source_id::text AS data_source_id, "
        "t.name, t.task_type, t.schedule_expr, t.parser_type, "
        "COALESCE(t.qc_profile, '') AS qc_profile, "
        "CASE WHEN t.enabled THEN 'true' ELSE 'false' END AS enabled, " +
        utc_column("t.created_at", "created_at") + ", " +
        utc_column("t.updated_at", "updated_at") + ", "
        "COALESCE(string_agg(tqr.qc_rule_id::text, ',' "
        "ORDER BY tqr.sort_order, tqr.qc_rule_id), '') AS qc_rule_ids ";
}

std::string task_run_columns() {
    return
        "tr.id::text AS id, tr.task_id::text AS task_id, "
        "n.node_code, tr.status, " +
        utc_column("tr.started_at", "started_at") + ", " +
        utc_column("tr.finished_at", "finished_at") + ", "
        "tr.items_total::text AS items_total, "
        "tr.items_success::text AS items_success, "
        "tr.items_failed::text AS items_failed, "
        "COALESCE(tr.error_summary, '') AS error_summary, "
        "tr.trigger_type, "
        "COALESCE(tr.execution_key, '') AS execution_key, " +
        utc_column("tr.scheduled_for", "scheduled_for") + " ";
}

}  // namespace

PostgresManagementQueryRepository::PostgresManagementQueryRepository(
    ISqlSession& session)
    : session_(session) {}

std::vector<NodeRecord>
PostgresManagementQueryRepository::list_nodes(
    const NodeListFilter& filter,
    const ManagementPageRequest& page) const {
    SqlParams params;
    std::string sql =
        "SELECT n.id::text AS id, n.node_code, n.name, n.status, "
        "COALESCE(n.agent_version, '') AS agent_version, " +
        utc_column("n.last_heartbeat_at", "last_heartbeat_at") + ", " +
        utc_column("n.created_at", "created_at") + ", " +
        utc_column("n.updated_at", "updated_at") +
        " FROM nodes n WHERE true";

    if (filter.effective_status.has_value()) {
        const auto now = bind(params, filter.now);
        const auto threshold =
            bind(params, std::to_string(filter.offline_after_seconds));
        const std::string online =
            "(n.status = 'online' AND n.last_heartbeat_at IS NOT NULL "
            "AND n.last_heartbeat_at <= " + now + "::timestamptz "
            "AND n.last_heartbeat_at >= " + now + "::timestamptz - (" +
            threshold + "::integer * interval '1 second'))";
        sql += *filter.effective_status ==
                       labbridge::core::NodeStatus::Online
                   ? " AND " + online
                   : " AND NOT " + online;
    }
    add_cursor(sql, params, "n.id", page);
    add_limit(sql, params, page);

    std::vector<NodeRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_node(row));
    }
    return records;
}

std::optional<NodeSummaryRecord>
PostgresManagementQueryRepository::find_node_summary(
    const std::string& node_code) const {
    const std::string sql =
        "SELECT n.id::text AS id, n.node_code, n.name, n.status, "
        "COALESCE(n.agent_version, '') AS agent_version, " +
        utc_column("n.last_heartbeat_at", "last_heartbeat_at") + ", " +
        utc_column("n.created_at", "created_at") + ", " +
        utc_column("n.updated_at", "updated_at") + ", "
        "(SELECT count(*) FROM tasks t "
        " WHERE t.node_id = n.id AND t.enabled)::text "
        "AS enabled_task_count, "
        "(SELECT count(*) FROM tasks t "
        " WHERE t.node_id = n.id AND NOT t.enabled)::text "
        "AS disabled_task_count, "
        "(SELECT count(*) FROM alerts a "
        " WHERE a.node_id = n.id AND a.status = 'open')::text "
        "AS open_alert_count, "
        "COALESCE(latest.id::text, '') AS latest_run_id, "
        "COALESCE(latest.task_id::text, '') AS latest_task_id, "
        "COALESCE(latest.status, '') AS latest_status, " +
        utc_column("latest.started_at", "latest_started_at") +
        " FROM nodes n "
        "LEFT JOIN LATERAL ("
        " SELECT tr.id, tr.task_id, tr.status, tr.started_at"
        " FROM task_runs tr WHERE tr.node_id = n.id"
        " ORDER BY tr.id DESC LIMIT 1"
        ") latest ON true "
        "WHERE n.node_code = $1 LIMIT 1";

    const auto row = session_.query_one(sql, {node_code});
    if (!row.has_value()) {
        return std::nullopt;
    }
    NodeSummaryRecord summary;
    summary.node = to_node(*row);
    summary.enabled_task_count =
        storage::int_or_zero(*row, "enabled_task_count");
    summary.disabled_task_count =
        storage::int_or_zero(*row, "disabled_task_count");
    summary.open_alert_count =
        storage::int_or_zero(*row, "open_alert_count");
    const auto latest_id =
        storage::value_or_empty(*row, "latest_run_id");
    if (!latest_id.empty()) {
        TaskRunRecord latest;
        latest.id = latest_id;
        latest.task_id =
            storage::value_or_empty(*row, "latest_task_id");
        latest.node_code = node_code;
        latest.status = storage::task_run_status_from_storage(
            storage::value_or_empty(*row, "latest_status"));
        latest.started_at =
            storage::value_or_empty(*row, "latest_started_at");
        summary.latest_task_run = std::move(latest);
    }
    return summary;
}

std::vector<DataSourceRecord>
PostgresManagementQueryRepository::list_data_sources_by_node(
    const std::string& node_code,
    const EnabledListFilter& filter,
    const ManagementPageRequest& page) const {
    SqlParams params{node_code};
    std::string sql =
        "SELECT ds.id::text AS id, n.node_code, ds.source_type, "
        "ds.name, ds.config_json::text AS config_json, "
        "CASE WHEN ds.enabled THEN 'true' ELSE 'false' END AS enabled, " +
        utc_column("ds.created_at", "created_at") + ", " +
        utc_column("ds.updated_at", "updated_at") +
        " FROM data_sources ds "
        "JOIN nodes n ON n.id = ds.node_id "
        "WHERE n.node_code = $1";
    if (filter.enabled.has_value()) {
        sql += " AND ds.enabled = " +
               bind(params, storage::bool_param(*filter.enabled)) +
               "::boolean";
    }
    add_cursor(sql, params, "ds.id", page);
    add_limit(sql, params, page);

    std::vector<DataSourceRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_data_source(row));
    }
    return records;
}

std::vector<QcRuleRecord>
PostgresManagementQueryRepository::list_qc_rules(
    const EnabledListFilter& filter,
    const ManagementPageRequest& page) const {
    SqlParams params;
    std::string sql =
        "SELECT qr.id::text AS id, qr.name, qr.rule_type, "
        "qr.rule_config_json::text AS rule_config_json, "
        "CASE WHEN qr.enabled THEN 'true' ELSE 'false' END AS enabled, " +
        utc_column("qr.created_at", "created_at") +
        " FROM qc_rules qr WHERE true";
    if (filter.enabled.has_value()) {
        sql += " AND qr.enabled = " +
               bind(params, storage::bool_param(*filter.enabled)) +
               "::boolean";
    }
    add_cursor(sql, params, "qr.id", page);
    add_limit(sql, params, page);

    std::vector<QcRuleRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_qc_rule(row));
    }
    return records;
}

std::vector<TaskRecord>
PostgresManagementQueryRepository::list_tasks_by_node(
    const std::string& node_code,
    const EnabledListFilter& filter,
    const ManagementPageRequest& page) const {
    SqlParams params{node_code};
    std::string sql =
        "SELECT " + task_columns() +
        "FROM tasks t "
        "JOIN nodes n ON n.id = t.node_id "
        "LEFT JOIN task_qc_rules tqr ON tqr.task_id = t.id "
        "WHERE n.node_code = $1";
    if (filter.enabled.has_value()) {
        sql += " AND t.enabled = " +
               bind(params, storage::bool_param(*filter.enabled)) +
               "::boolean";
    }
    add_cursor(sql, params, "t.id", page);
    sql += " GROUP BY t.id, n.node_code";
    add_limit(sql, params, page);

    std::vector<TaskRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_task(row));
    }
    return records;
}

std::optional<TaskRecord>
PostgresManagementQueryRepository::find_task(
    const std::string& task_id) const {
    const std::string sql =
        "SELECT " + task_columns() +
        "FROM tasks t "
        "JOIN nodes n ON n.id = t.node_id "
        "LEFT JOIN task_qc_rules tqr ON tqr.task_id = t.id "
        "WHERE t.id = $1::bigint "
        "GROUP BY t.id, n.node_code LIMIT 1";
    const auto row = session_.query_one(sql, {task_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_task(*row);
}

std::vector<TaskRunRecord>
PostgresManagementQueryRepository::list_task_runs_by_node(
    const TaskRunListFilter& filter,
    const ManagementPageRequest& page) const {
    SqlParams params{filter.node_code};
    std::string sql =
        "SELECT " + task_run_columns() +
        "FROM task_runs tr "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE n.node_code = $1";
    if (filter.task_id.has_value()) {
        sql += " AND tr.task_id = " +
               bind(params, *filter.task_id) + "::bigint";
    }
    if (filter.status.has_value()) {
        sql += " AND tr.status = " +
               bind(params, storage::to_storage(*filter.status));
    }
    add_cursor(sql, params, "tr.id", page);
    add_limit(sql, params, page);

    std::vector<TaskRunRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_task_run(row));
    }
    return records;
}

std::optional<TaskRunSummaryRecord>
PostgresManagementQueryRepository::find_task_run_summary(
    const std::string& task_run_id) const {
    const std::string sql =
        "SELECT " + task_run_columns() + ", "
        "(SELECT count(*) FROM raw_files rf "
        " WHERE rf.task_run_id = tr.id)::text AS raw_file_count, "
        "(SELECT count(*) FROM parsed_records pr "
        " WHERE pr.task_run_id = tr.id)::text AS parsed_record_count, "
        "(SELECT count(*) FROM qc_results qr "
        " JOIN parsed_records pr ON pr.id = qr.parsed_record_id "
        " WHERE pr.task_run_id = tr.id)::text AS qc_result_count, "
        "(SELECT count(*) FROM alerts a "
        " WHERE a.task_run_id = tr.id)::text AS alert_count "
        "FROM task_runs tr "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE tr.id = $1::bigint LIMIT 1";
    const auto row = session_.query_one(sql, {task_run_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    TaskRunSummaryRecord summary;
    summary.task_run = to_task_run(*row);
    summary.raw_file_count =
        storage::int_or_zero(*row, "raw_file_count");
    summary.parsed_record_count =
        storage::int_or_zero(*row, "parsed_record_count");
    summary.qc_result_count =
        storage::int_or_zero(*row, "qc_result_count");
    summary.alert_count =
        storage::int_or_zero(*row, "alert_count");
    return summary;
}

std::vector<RawFileRecord>
PostgresManagementQueryRepository::list_raw_files_by_run(
    const std::string& task_run_id,
    const ManagementPageRequest& page) const {
    SqlParams params{task_run_id};
    std::string sql =
        "SELECT rf.id::text AS id, "
        "rf.task_run_id::text AS task_run_id, n.node_code, "
        "rf.original_name, COALESCE(rf.file_hash, '') AS file_hash, "
        "rf.storage_path, rf.size_bytes::text AS size_bytes, " +
        utc_column("rf.source_mtime", "source_mtime") + ", "
        "rf.ingest_status, " +
        utc_column("rf.created_at", "created_at") +
        " FROM raw_files rf "
        "JOIN nodes n ON n.id = rf.node_id "
        "WHERE rf.task_run_id = $1::bigint";
    add_cursor(sql, params, "rf.id", page);
    add_limit(sql, params, page);

    std::vector<RawFileRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_raw_file(row));
    }
    return records;
}

std::vector<ParsedRecordRecord>
PostgresManagementQueryRepository::list_parsed_records_by_run(
    const std::string& task_run_id,
    const ManagementPageRequest& page) const {
    SqlParams params{task_run_id};
    std::string sql =
        "SELECT pr.id::text AS id, "
        "COALESCE(pr.raw_file_id::text, '') AS raw_file_id, "
        "pr.task_run_id::text AS task_run_id, "
        "COALESCE(pr.station_code, '') AS station_code, "
        "COALESCE(pr.device_code, '') AS device_code, " +
        utc_column("pr.record_time", "record_time") + ", "
        "pr.payload_json::text AS payload_json, pr.parse_status, " +
        utc_column("pr.created_at", "created_at") +
        " FROM parsed_records pr "
        "WHERE pr.task_run_id = $1::bigint";
    add_cursor(sql, params, "pr.id", page);
    add_limit(sql, params, page);

    std::vector<ParsedRecordRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_parsed_record(row));
    }
    return records;
}

std::vector<QcResultRecord>
PostgresManagementQueryRepository::list_qc_results_by_run(
    const QcResultListFilter& filter,
    const ManagementPageRequest& page) const {
    SqlParams params{filter.task_run_id};
    std::string sql =
        "SELECT qr.id::text AS id, "
        "qr.parsed_record_id::text AS parsed_record_id, "
        "qr.qc_rule_id::text AS qc_rule_id, "
        "pr.task_run_id::text AS task_run_id, "
        "qr.level, qr.result, COALESCE(qr.message, '') AS message, " +
        utc_column("qr.created_at", "created_at") +
        " FROM qc_results qr "
        "JOIN parsed_records pr ON pr.id = qr.parsed_record_id "
        "WHERE pr.task_run_id = $1::bigint";
    if (filter.result.has_value()) {
        sql += " AND qr.result = " + bind(params, *filter.result);
    }
    add_cursor(sql, params, "qr.id", page);
    add_limit(sql, params, page);

    std::vector<QcResultRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_qc_result(row));
    }
    return records;
}

std::vector<AlertRecord>
PostgresManagementQueryRepository::list_alerts_by_node(
    const AlertListFilter& filter,
    const ManagementPageRequest& page) const {
    SqlParams params{filter.node_code};
    std::string sql =
        "SELECT a.id::text AS id, n.node_code, "
        "COALESCE(a.task_run_id::text, '') AS task_run_id, "
        "a.alert_type, a.severity, a.message, a.status, " +
        utc_column("a.created_at", "created_at") +
        " FROM alerts a "
        "JOIN nodes n ON n.id = a.node_id "
        "WHERE n.node_code = $1";
    if (filter.task_run_id.has_value()) {
        sql += " AND a.task_run_id = " +
               bind(params, *filter.task_run_id) + "::bigint";
    }
    if (filter.status.has_value()) {
        sql += " AND a.status = " + bind(params, *filter.status);
    }
    if (filter.severity.has_value()) {
        sql += " AND a.severity = " +
               bind(params, *filter.severity);
    }
    add_cursor(sql, params, "a.id", page);
    add_limit(sql, params, page);

    std::vector<AlertRecord> records;
    for (const auto& row : session_.query_all(sql, params)) {
        records.push_back(to_alert(row));
    }
    return records;
}

}  // namespace labbridge::server
