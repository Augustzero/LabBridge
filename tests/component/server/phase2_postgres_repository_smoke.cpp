#include "labbridge/core/version.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/application/node_service.h"

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

struct RecordedStatement {
    std::string sql;
    labbridge::server::SqlParams params;
};

class RecordingSqlSession final : public labbridge::server::ISqlSession {
public:
    void execute(const std::string& sql, const labbridge::server::SqlParams& params) override {
        executed.push_back({sql, params});
        row["node_code"] = params[0];
        row["name"] = params[1];
        row["status"] = params[2];
        row["agent_version"] = params[3];
        row["last_heartbeat_at"] = params[4];
    }

    std::optional<labbridge::server::SqlRow> query_one(const std::string& sql,
                                                       const labbridge::server::SqlParams& params) override {
        queried.push_back({sql, params});
        if (row.empty() || row["node_code"] != params[0]) {
            return std::nullopt;
        }
        return row;
    }

    std::vector<labbridge::server::SqlRow> query_all(const std::string& sql,
                                                     const labbridge::server::SqlParams& params) override {
        queried.push_back({sql, params});
        if (row.empty() || row["node_code"] != params[0]) {
            return {};
        }
        return {row};
    }

    std::vector<RecordedStatement> executed;
    std::vector<RecordedStatement> queried;
    labbridge::server::SqlRow row;
};

}  // namespace

int main() {
    RecordingSqlSession session;
    labbridge::server::PostgresNodeRepository repository(session);
    labbridge::server::NodeService node_service(repository);

    const auto register_status = node_service.register_node({
        "lab-node-pg-001",
        "postgres-backed-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);
    assert(session.executed.size() == 1);
    assert(session.executed.back().sql.find("INSERT INTO nodes") != std::string::npos);
    assert(session.executed.back().sql.find("ON CONFLICT (node_code)") != std::string::npos);
    assert(session.executed.back().params[2] == "offline");

    const auto heartbeat_status = node_service.accept_heartbeat({
        "lab-node-pg-001",
        labbridge::core::kVersion,
        "2026-05-15 10:30:00",
    });
    assert(heartbeat_status.ok);
    assert(session.queried.size() == 1);
    assert(session.executed.size() == 2);
    assert(session.executed.back().params[2] == "online");
    assert(session.executed.back().params[4] == "2026-05-15 10:30:00");

    const auto node = node_service.find_node("lab-node-pg-001");
    assert(node.has_value());
    assert(node->status == labbridge::core::NodeStatus::Online);
    assert(node->last_heartbeat_at == "2026-05-15 10:30:00");

    return 0;
}
