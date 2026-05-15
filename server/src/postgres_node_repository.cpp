#include "labbridge/server/postgres_node_repository.h"

#include <utility>

namespace labbridge::server {
namespace {

std::string to_storage_status(labbridge::core::NodeStatus status) {
    switch (status) {
        case labbridge::core::NodeStatus::Online:
            return "online";
        case labbridge::core::NodeStatus::Offline:
            return "offline";
    }
    return "offline";
}

labbridge::core::NodeStatus from_storage_status(const std::string& status) {
    if (status == "online") {
        return labbridge::core::NodeStatus::Online;
    }
    return labbridge::core::NodeStatus::Offline;
}

std::string get_or_empty(const SqlRow& row, const std::string& key) {
    const auto iter = row.find(key);
    if (iter == row.end()) {
        return {};
    }
    return iter->second;
}

}  // namespace

PostgresNodeRepository::PostgresNodeRepository(ISqlSession& session) : session_(session) {}

void PostgresNodeRepository::upsert(NodeRecord node) {
    static const std::string sql =
        "INSERT INTO nodes "
        "(node_code, name, status, agent_version, last_heartbeat_at) "
        "VALUES ($1, $2, $3, $4, NULLIF($5, '')::timestamptz) "
        "ON CONFLICT (node_code) DO UPDATE SET "
        "name = EXCLUDED.name, "
        "status = EXCLUDED.status, "
        "agent_version = EXCLUDED.agent_version, "
        "last_heartbeat_at = EXCLUDED.last_heartbeat_at, "
        "updated_at = now()";

    session_.execute(sql,
                     {
                         node.info.node_code,
                         node.info.name,
                         to_storage_status(node.status),
                         node.info.agent_version,
                         node.last_heartbeat_at,
                     });
}

std::optional<NodeRecord> PostgresNodeRepository::find_by_code(const std::string& node_code) const {
    static const std::string sql =
        "SELECT node_code, name, status, agent_version, "
        "COALESCE(to_char(last_heartbeat_at, 'YYYY-MM-DD HH24:MI:SS'), '') AS last_heartbeat_at "
        "FROM nodes WHERE node_code = $1 LIMIT 1";

    const auto row = session_.query_one(sql, {node_code});
    if (!row.has_value()) {
        return std::nullopt;
    }

    NodeRecord record;
    record.info.node_code = get_or_empty(*row, "node_code");
    record.info.name = get_or_empty(*row, "name");
    record.info.agent_version = get_or_empty(*row, "agent_version");
    record.status = from_storage_status(get_or_empty(*row, "status"));
    record.last_heartbeat_at = get_or_empty(*row, "last_heartbeat_at");
    return record;
}

}  // namespace labbridge::server
