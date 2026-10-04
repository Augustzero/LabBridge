#include "labbridge/server/postgres/alert_repository.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <stdexcept>
#include <utility>

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
    alert.acknowledged_at = storage::value_or_empty(row, "acknowledged_at");
    alert.closed_at = storage::value_or_empty(row, "closed_at");
    return alert;
}

// 告警完整行投影；node 侧用 LEFT JOIN，node_id 理论上可空。
std::string alert_select() {
    return "SELECT a.id::text AS id, COALESCE(n.node_code, '') AS node_code, "
           "COALESCE(a.task_run_id::text, '') AS task_run_id, "
           "a.alert_type, a.severity, a.message, a.status, " +
           storage::utc_column("a.acknowledged_at", "acknowledged_at") + ", " +
           storage::utc_column("a.closed_at", "closed_at") +
           " FROM alerts a "
           "LEFT JOIN nodes n ON n.id = a.node_id ";
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

std::vector<AlertRecord> PostgresAlertRepository::find_by_node(
    const std::string& node_code) const {
    static const std::string sql =
        alert_select() +
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
        alert_select() +
        "WHERE a.task_run_id = $1::bigint "
        "ORDER BY a.id";

    std::vector<AlertRecord> alerts;
    for (const auto& row : session_.query_all(sql, {task_run_id})) {
        alerts.push_back(to_alert_record(row));
    }
    return alerts;
}

std::optional<AlertRecord> PostgresAlertRepository::lock_by_id(
    const std::string& alert_id) {
    // 只锁 alerts 行：node 侧是外连接的可空端，FOR UPDATE 不允许带上它。
    static const std::string sql = alert_select() +
                                   "WHERE a.id = $1::bigint "
                                   "FOR UPDATE OF a";
    const auto row = session_.query_one(sql, {alert_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_alert_record(*row);
}

std::optional<AlertRecord> PostgresAlertRepository::save_acknowledged(
    const std::string& alert_id) {
    static const std::string sql =
        "UPDATE alerts SET status = 'acknowledged', "
        "acknowledged_at = COALESCE(acknowledged_at, now()) "
        "WHERE id = $1::bigint "
        "RETURNING id::text AS id";
    if (!session_.query_one(sql, {alert_id}).has_value()) {
        return std::nullopt;
    }
    return read_by_id(alert_id);
}

std::optional<AlertRecord> PostgresAlertRepository::save_closed(
    const std::string& alert_id) {
    // 直接关闭不动 acknowledged_at：没确认过就关闭的告警，确认时间保持空。
    static const std::string sql =
        "UPDATE alerts SET status = 'closed', "
        "closed_at = COALESCE(closed_at, now()) "
        "WHERE id = $1::bigint "
        "RETURNING id::text AS id";
    if (!session_.query_one(sql, {alert_id}).has_value()) {
        return std::nullopt;
    }
    return read_by_id(alert_id);
}

std::optional<AlertRecord> PostgresAlertRepository::read_by_id(
    const std::string& alert_id) const {
    static const std::string sql =
        alert_select() + "WHERE a.id = $1::bigint";
    const auto row = session_.query_one(sql, {alert_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_alert_record(*row);
}

}  // namespace labbridge::server
