#include "labbridge/core/version.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"

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
        node_row["node_code"] = params[0];
        node_row["name"] = params[1];
        node_row["status"] = params[2];
        node_row["agent_version"] = params[3];
        node_row["last_heartbeat_at"] = params[4];
    }

    std::optional<labbridge::server::SqlRow> query_one(
        const std::string& sql,
        const labbridge::server::SqlParams& params) override {
        queried.push_back({sql, params});

        if (sql.find("INSERT INTO data_sources") != std::string::npos) {
            data_source_row["id"] = "101";
            data_source_row["node_code"] = params[0];
            data_source_row["source_type"] = params[1];
            data_source_row["name"] = params[2];
            data_source_row["config_json"] = params[3];
            data_source_row["enabled"] = params[4];
            return labbridge::server::SqlRow{{"id", "101"}};
        }

        if (sql.find("FROM data_sources") != std::string::npos) {
            if (data_source_row.empty() || data_source_row["id"] != params[0]) {
                return std::nullopt;
            }
            return data_source_row;
        }

        if (sql.find("INSERT INTO tasks") != std::string::npos) {
            task_row["id"] = "202";
            task_row["node_code"] = params[0];
            task_row["data_source_id"] = params[1];
            task_row["name"] = params[2];
            task_row["task_type"] = params[3];
            task_row["schedule_expr"] = params[4];
            task_row["parser_type"] = params[5];
            task_row["qc_profile"] = params[6];
            task_row["enabled"] = params[7];
            return labbridge::server::SqlRow{{"id", "202"}};
        }

        if (sql.find("FROM nodes WHERE node_code") != std::string::npos) {
            if (node_row.empty() || node_row["node_code"] != params[0]) {
                return std::nullopt;
            }
            return node_row;
        }

        return std::nullopt;
    }

    std::vector<labbridge::server::SqlRow> query_all(
        const std::string& sql,
        const labbridge::server::SqlParams& params) override {
        queried.push_back({sql, params});
        if (sql.find("FROM tasks") == std::string::npos || task_row.empty()) {
            return {};
        }
        if (task_row["node_code"] != params[0] || task_row["enabled"] != "true") {
            return {};
        }
        return {task_row};
    }

    std::vector<RecordedStatement> executed;
    std::vector<RecordedStatement> queried;
    labbridge::server::SqlRow node_row;
    labbridge::server::SqlRow data_source_row;
    labbridge::server::SqlRow task_row;
};

}  // namespace

int main() {
    RecordingSqlSession session;
    labbridge::server::PostgresNodeRepository node_repository(session);
    labbridge::server::PostgresConfigRepository config_repository(session);
    labbridge::server::NodeService node_service(node_repository);
    labbridge::server::ConfigService config_service(node_repository, config_repository);

    const auto register_status = node_service.register_node({
        "lab-node-config-005",
        "config-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);

    const auto missing_node_source = config_service.create_data_source({
        "missing-node",
        labbridge::core::SourceType::LocalDirectory,
        "local csv dir",
        R"({"path":"tests/fixtures/agent","pattern":"*.csv"})",
        true,
    });
    assert(!missing_node_source.status.ok);

    const auto data_source = config_service.create_data_source({
        "lab-node-config-005",
        labbridge::core::SourceType::LocalDirectory,
        "local csv dir",
        R"({"path":"tests/fixtures/agent","pattern":"*.csv"})",
        true,
    });
    assert(data_source.status.ok);
    assert(data_source.id == "101");
    assert(session.queried.back().sql.find("INSERT INTO data_sources") != std::string::npos);
    assert(session.queried.back().params[1] == "local_directory");

    const auto task = config_service.create_task({
        "lab-node-config-005",
        data_source.id,
        "collect local csv",
        "collect_parse_qc",
        "*/5 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);
    assert(task.id == "202");
    assert(session.queried.back().sql.find("INSERT INTO tasks") != std::string::npos);

    const auto tasks = config_service.find_enabled_tasks("lab-node-config-005");
    assert(tasks.size() == 1);
    assert(tasks.front().data_source_id == data_source.id);
    assert(tasks.front().parser_type == "csv_observation");
    assert(tasks.front().qc_profile == "basic");

    const auto second_register_status = node_service.register_node({
        "lab-node-config-006",
        "second-config-node",
        labbridge::core::kVersion,
    });
    assert(second_register_status.ok);

    const auto cross_node_task = config_service.create_task({
        "lab-node-config-006",
        data_source.id,
        "invalid cross node task",
        "collect_parse_qc",
        "*/10 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    assert(!cross_node_task.status.ok);

    return 0;
}
