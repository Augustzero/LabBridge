#include "labbridge/server/postgres_alert_repository.h"
#include "labbridge/server/storage_mapping.h"

#include <stdexcept>

namespace labbridge::server {
namespace {

AlertRecord to_alert_record(const SqlRow& row) {
    AlertRecord alert;
    alert.id = storage::value_or_empty(row, "id");
    alert.node_code = storage::value_or_empty(row, "node_code");
    alert.task_run_id = storage::value_or_empty(row, "task_run_id");
    alert.alert_type = storage::value_or_empty(row, "alert_type");
    alert.severity = storage::value_or_empty(row, "severity");
    alert.message = storage::value_or_empty(row, "message");
    alert.status = storage::value_or_empty(row, "status");
    return alert;
}

}  // namespace

PostgresAlertRepository::PostgresAlertRepository(ISqlSession& session) : session_(session) {}

std::string PostgresAlertRepository::create(AlertRecord alert) {
    static const std::string sql =
        "INSERT INTO alerts (node_id, task_run_id, alert_type, severity, message, status) "
        "SELECT n.id, NULLIF($2, '')::bigint, $3, $4, $5, $6 "
        "FROM nodes n "
        "LEFT JOIN task_runs tr ON tr.id = NULLIF($2, '')::bigint AND tr.node_id = n.id "
        "WHERE n.node_code = $1 AND ($2 = '' OR tr.id IS NOT NULL) "
        "RETURNING id::text AS id";

    const auto status = alert.status.empty() ? "open" : alert.status;
    const auto row = session_.query_one(sql,
                                        {
                                            alert.node_code,
                                            alert.task_run_id,
                                            alert.alert_type,
                                            alert.severity,
                                            alert.message,
                                            status,
                                        });
    if (!row.has_value()) {
        throw std::runtime_error("failed to create alert");
    }
    return storage::value_or_empty(*row, "id");
}

std::optional<AlertRecord> PostgresAlertRepository::find_by_id(
    const std::string& alert_id) const {
    static const std::string sql =
        "SELECT a.id::text AS id, COALESCE(n.node_code, '') AS node_code, "
        "COALESCE(a.task_run_id::text, '') AS task_run_id, a.alert_type, a.severity, "
        "a.message, a.status "
        "FROM alerts a "
        "LEFT JOIN nodes n ON n.id = a.node_id "
        "WHERE a.id = $1::bigint "
        "LIMIT 1";

    const auto row = session_.query_one(sql, {alert_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_alert_record(*row);
}

std::vector<AlertRecord> PostgresAlertRepository::find_by_node(
    const std::string& node_code) const {
    static const std::string sql =
        "SELECT a.id::text AS id, n.node_code, COALESCE(a.task_run_id::text, '') AS task_run_id, "
        "a.alert_type, a.severity, a.message, a.status "
        "FROM alerts a "
        "JOIN nodes n ON n.id = a.node_id "
        "WHERE n.node_code = $1 "
        "ORDER BY a.id";

    std::vector<AlertRecord> alerts;
    for (const auto& row : session_.query_all(sql, {node_code})) {
        alerts.push_back(to_alert_record(row));
    }
    return alerts;
}

std::vector<AlertRecord> PostgresAlertRepository::find_by_task_run(
    const std::string& task_run_id) const {
    static const std::string sql =
        "SELECT a.id::text AS id, COALESCE(n.node_code, '') AS node_code, "
        "COALESCE(a.task_run_id::text, '') AS task_run_id, a.alert_type, a.severity, "
        "a.message, a.status "
        "FROM alerts a "
        "LEFT JOIN nodes n ON n.id = a.node_id "
        "WHERE a.task_run_id = $1::bigint "
        "ORDER BY a.id";

    std::vector<AlertRecord> alerts;
    for (const auto& row : session_.query_all(sql, {task_run_id})) {
        alerts.push_back(to_alert_record(row));
    }
    return alerts;
}

}  // namespace labbridge::server
