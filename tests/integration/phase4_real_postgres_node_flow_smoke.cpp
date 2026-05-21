#include "labbridge/core/version.h"
#include "labbridge/server/libpq_sql_session.h"
#include "labbridge/server/node_service.h"
#include "labbridge/server/postgres_node_repository.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL smoke test\n";
        return 0;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository repository{session};
    labbridge::server::NodeService node_service{repository};

    const std::string node_code = "lab-node-real-pg-004";

    const auto register_status = node_service.register_node({
        node_code,
        "real-postgres-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);

    const auto registered_node = node_service.find_node(node_code);
    assert(registered_node.has_value());
    assert(registered_node->info.node_code == node_code);
    assert(registered_node->info.name == "real-postgres-node");
    assert(registered_node->status == labbridge::core::NodeStatus::Offline);

    const auto heartbeat_status = node_service.accept_heartbeat({
        node_code,
        labbridge::core::kVersion,
        "2026-05-18 10:30:00+08",
    });
    assert(heartbeat_status.ok);

    const auto heartbeat_node = node_service.find_node(node_code);
    assert(heartbeat_node.has_value());
    assert(heartbeat_node->info.node_code == node_code);
    assert(heartbeat_node->info.agent_version == labbridge::core::kVersion);
    assert(heartbeat_node->status == labbridge::core::NodeStatus::Online);
    assert(!heartbeat_node->last_heartbeat_at.empty());

    return 0;
}
