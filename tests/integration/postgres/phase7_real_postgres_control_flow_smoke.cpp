#include "labbridge/core/version.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/postgres/storage_mapping.h"
#include "labbridge/server/application/task_run_service.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL control flow smoke test\n";
        return 77;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{node_repository, config_repository};
    labbridge::server::TaskRunService task_run_service{config_repository, task_run_repository};

    const std::string node_code = "lab-node-real-flow-007";

    const auto register_status = node_service.register_node({
        node_code,
        "real-control-flow-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);

    const auto heartbeat_status = node_service.accept_heartbeat({
        node_code,
        labbridge::core::kVersion,
        "2026-05-25 10:00:00+08",
    });
    assert(heartbeat_status.ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase7 local csv dir",
        R"({"path":"tests/fixtures/agent","pattern":"*.csv"})",
        true,
    });
    assert(data_source.status.ok);
    assert(!data_source.id.empty());

    const auto stored_data_source = config_repository.find_data_source(data_source.id);
    assert(stored_data_source.has_value());
    assert(stored_data_source->node_code == node_code);
    assert(stored_data_source->name == "phase7 local csv dir");

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase7 collect local csv",
        "collect_parse_qc",
        "* * * * *",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);
    assert(!task.id.empty());

    const auto stored_task = config_repository.find_task(task.id);
    assert(stored_task.has_value());
    assert(stored_task->node_code == node_code);
    assert(stored_task->data_source_id == data_source.id);

    const auto enabled_tasks = config_service.find_enabled_tasks(node_code);
    bool found_task = false;
    for (const auto& enabled_task : enabled_tasks) {
        if (enabled_task.id == task.id) {
            found_task = true;
            break;
        }
    }
    assert(found_task);

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-05-25 10:01:00+08",
        "manual",
    });
    assert(started.status.ok);
    assert(!started.id.empty());

    const auto running_run = task_run_service.find_run(started.id);
    assert(running_run.has_value());
    assert(running_run->status == labbridge::core::TaskRunStatus::Running);

    const auto finish_status = task_run_service.finish({
        started.id,
        labbridge::core::TaskRunStatus::Succeeded,
        "2026-05-25 10:02:00+08",
        2,
        2,
        0,
        "",
    });
    assert(finish_status.ok);

    const auto finished_run = task_run_service.find_run(started.id);
    assert(finished_run.has_value());
    assert(finished_run->status == labbridge::core::TaskRunStatus::Succeeded);
    assert(finished_run->items_total == 2);
    assert(finished_run->items_success == 2);
    assert(finished_run->items_failed == 0);

    const auto persisted = session.query_one(
        "SELECT n.node_code, ds.id::text AS data_source_id, t.id::text AS task_id, "
        "tr.id::text AS task_run_id, tr.status "
        "FROM task_runs tr "
        "JOIN tasks t ON t.id = tr.task_id "
        "JOIN data_sources ds ON ds.id = t.data_source_id "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE n.node_code = $1 AND ds.id = $2::bigint AND t.id = $3::bigint AND tr.id = $4::bigint "
        "LIMIT 1",
        {node_code, data_source.id, task.id, started.id});

    assert(persisted.has_value());
    assert(labbridge::server::storage::value_or_empty(*persisted, "node_code") == node_code);
    assert(labbridge::server::storage::value_or_empty(*persisted, "data_source_id") ==
           data_source.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "task_id") == task.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "task_run_id") == started.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "status") == "succeeded");

    return 0;
}
