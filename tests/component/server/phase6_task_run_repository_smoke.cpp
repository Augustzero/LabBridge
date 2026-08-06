#include "labbridge/core/version.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/application/task_run_service.h"

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

        if (sql.find("UPDATE task_runs") != std::string::npos) {
            task_run_row["status"] = params[1];
            task_run_row["finished_at"] = params[2];
            task_run_row["items_total"] = params[3];
            task_run_row["items_success"] = params[4];
            task_run_row["items_failed"] = params[5];
            task_run_row["error_summary"] = params[6];
            return;
        }

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
            data_source_row["id"] = "301";
            data_source_row["node_code"] = params[0];
            data_source_row["source_type"] = params[1];
            data_source_row["name"] = params[2];
            data_source_row["config_json"] = params[3];
            data_source_row["enabled"] = params[4];
            return labbridge::server::SqlRow{{"id", "301"}};
        }

        if (sql.find("FROM data_sources ds") != std::string::npos) {
            if (data_source_row.empty() || data_source_row["id"] != params[0]) {
                return std::nullopt;
            }
            return data_source_row;
        }

        if (sql.find("INSERT INTO tasks") != std::string::npos) {
            task_row["id"] = "401";
            task_row["node_code"] = params[0];
            task_row["data_source_id"] = params[1];
            task_row["name"] = params[2];
            task_row["task_type"] = params[3];
            task_row["schedule_expr"] = params[4];
            task_row["parser_type"] = params[5];
            task_row["qc_profile"] = params[6];
            task_row["enabled"] = params[7];
            return labbridge::server::SqlRow{{"id", "401"}};
        }

        if (sql.find("INSERT INTO task_runs") != std::string::npos) {
            task_run_row["id"] = "501";
            task_run_row["task_id"] = params[0];
            task_run_row["node_code"] = params[1];
            task_run_row["status"] = params[2];
            task_run_row["started_at"] = params[3];
            task_run_row["finished_at"] = "";
            task_run_row["items_total"] = params[4];
            task_run_row["items_success"] = params[5];
            task_run_row["items_failed"] = params[6];
            task_run_row["error_summary"] = params[7];
            task_run_row["trigger_type"] = params[8];
            return labbridge::server::SqlRow{{"id", "501"}};
        }

        if (sql.find("FROM task_runs tr") != std::string::npos) {
            if (task_run_row.empty() || task_run_row["id"] != params[0]) {
                return std::nullopt;
            }
            return task_run_row;
        }

        if (sql.find("FROM tasks t") != std::string::npos && sql.find("WHERE t.id") != std::string::npos) {
            if (task_row.empty() || task_row["id"] != params[0]) {
                return std::nullopt;
            }
            return task_row;
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
    labbridge::server::SqlRow task_run_row;
};

}  // namespace

int main() {
    RecordingSqlSession session;
    labbridge::server::PostgresNodeRepository node_repository(session);
    labbridge::server::PostgresConfigRepository config_repository(session);
    labbridge::server::PostgresTaskRunRepository task_run_repository(session);
    labbridge::server::NodeService node_service(node_repository);
    labbridge::server::ConfigService config_service(node_repository, config_repository);
    labbridge::server::TaskRunService task_run_service(config_repository, task_run_repository);

    const auto register_status = node_service.register_node({
        "lab-node-run-006",
        "task-run-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);

    const auto data_source = config_service.create_data_source({
        "lab-node-run-006",
        labbridge::core::SourceType::LocalDirectory,
        "local csv dir",
        R"({"path":"tests/fixtures/agent","pattern":"*.csv"})",
        true,
    });
    assert(data_source.status.ok);

    const auto task = config_service.create_task({
        "lab-node-run-006",
        data_source.id,
        "collect local csv",
        "collect_parse_qc",
        "*/5 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);

    const auto wrong_node_run = task_run_service.start({
        "other-node",
        task.id,
        "2026-05-21 10:00:00+08",
        "manual",
    });
    assert(!wrong_node_run.status.ok);

    const auto started = task_run_service.start({
        "lab-node-run-006",
        task.id,
        "2026-05-21 10:00:00+08",
        "manual",
    });
    assert(started.status.ok);
    assert(started.id == "501");
    assert(session.queried.back().sql.find("INSERT INTO task_runs") != std::string::npos);
    assert(session.queried.back().params[2] == "running");
    assert(session.queried.back().params[8] == "manual");

    const auto running_run = task_run_service.find_run(started.id);
    assert(running_run.has_value());
    assert(running_run->status == labbridge::core::TaskRunStatus::Running);

    const auto invalid_finish = task_run_service.finish({
        started.id,
        labbridge::core::TaskRunStatus::Running,
        "2026-05-21 10:01:00+08",
        2,
        2,
        0,
        "",
    });
    assert(!invalid_finish.ok);

    const auto finish_status = task_run_service.finish({
        started.id,
        labbridge::core::TaskRunStatus::Succeeded,
        "2026-05-21 10:01:00+08",
        2,
        2,
        0,
        "",
    });
    assert(finish_status.ok);
    assert(session.executed.back().sql.find("UPDATE task_runs") != std::string::npos);
    assert(session.executed.back().params[1] == "succeeded");

    const auto finished_run = task_run_service.find_run(started.id);
    assert(finished_run.has_value());
    assert(finished_run->status == labbridge::core::TaskRunStatus::Succeeded);
    assert(finished_run->items_total == 2);
    assert(finished_run->items_success == 2);
    assert(finished_run->items_failed == 0);

    return 0;
}
