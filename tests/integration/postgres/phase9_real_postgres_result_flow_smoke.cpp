#include "labbridge/core/version.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/postgres/storage_mapping.h"
#include "labbridge/server/application/task_run_service.h"
#include "support/server/test_config_seed.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL result flow smoke test\n";
        return 77;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::PostgresResultRepository result_repository{session};
    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{config_repository};
    labbridge::server::TaskRunService task_run_service{config_repository, task_run_repository};
    labbridge::server::ResultService result_service{task_run_repository, result_repository};

    const std::string node_code = "lab-node-real-result-009";

    const auto register_status = node_service.register_node({
        node_code,
        "real-result-flow-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);

    const auto heartbeat_status = node_service.accept_heartbeat({
        node_code,
        labbridge::core::kVersion,
        "2026-05-26T02:00:00Z",
    });
    assert(heartbeat_status.ok);

    const auto data_source =
        labbridge::server::test_support::create_local_csv_data_source(
            config_repository, node_code, "phase9 local csv dir",
            R"({"path":"tests/fixtures/agent","pattern":"*.csv"})");
    assert(!data_source.empty());

    const auto task = labbridge::server::test_support::create_csv_task(
        config_repository, node_code, data_source,
        "phase9 collect local csv");
    assert(!task.empty());

    const auto started = task_run_service.start({
        node_code,
        task,
        "2026-05-26T02:01:00Z",
        "manual",
    });
    assert(started.status.ok);
    assert(!started.id.empty());

    const auto raw_file = result_service.record_raw_file({
        started.id,
        node_code,
        "sample_observation.csv",
        "phase9-hash-001",
        "/archive/phase9/sample_observation.csv",
        128,
        "2026-05-26T01:59:00Z",
        "collected",
    });
    assert(raw_file.status.ok);
    assert(!raw_file.id.empty());

    const auto stored_raw_file = result_repository.find_raw_file(raw_file.id);
    assert(stored_raw_file.has_value());
    assert(stored_raw_file->task_run_id == started.id);
    assert(stored_raw_file->node_code == node_code);
    assert(stored_raw_file->original_name == "sample_observation.csv");

    const auto record_parsed = [&](const std::string& device_code) {
        return result_service.record_parsed_record(
            {
                started.id,
                raw_file.id,
                {
                    "station-a",
                    device_code,
                    "2026-05-26T02:00:00Z",
                    R"({"temperature":21.5,"humidity":62})",
                },
                "parsed",
            },
            *stored_raw_file);
    };

    const auto first_record = record_parsed("device-a");
    assert(first_record.status.ok);
    assert(!first_record.id.empty());

    const auto second_record = record_parsed("device-b");
    assert(second_record.status.ok);
    assert(!second_record.id.empty());

    const auto persisted_first =
        result_repository.find_parsed_record(first_record.id);
    assert(persisted_first.has_value());
    assert(persisted_first->raw_file_id == raw_file.id);
    assert(persisted_first->record.device_code == "device-a");
    assert(persisted_first->record.payload_json.find("temperature") !=
           std::string::npos);

    const auto persisted_second =
        result_repository.find_parsed_record(second_record.id);
    assert(persisted_second.has_value());
    assert(persisted_second->raw_file_id == raw_file.id);
    assert(persisted_second->record.device_code == "device-b");
    assert(persisted_second->record.payload_json.find("humidity") !=
           std::string::npos);

    const auto finish_status = task_run_service.finish({
        started.id,
        labbridge::core::TaskRunStatus::Succeeded,
        "2026-05-26T02:06:00Z",
        2,
        2,
        0,
        "",
    });
    assert(finish_status.ok);

    const auto persisted = session.query_one(
        "SELECT n.node_code, rf.id::text AS raw_file_id, pr.id::text AS parsed_record_id, "
        "pr.device_code, pr.payload_json::text AS payload_json, tr.status "
        "FROM parsed_records pr "
        "JOIN raw_files rf ON rf.id = pr.raw_file_id "
        "JOIN task_runs tr ON tr.id = pr.task_run_id "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE n.node_code = $1 AND rf.id = $2::bigint AND pr.id = $3::bigint "
        "LIMIT 1",
        {node_code, raw_file.id, first_record.id});

    assert(persisted.has_value());
    assert(labbridge::server::storage::value_or_empty(*persisted, "node_code") == node_code);
    assert(labbridge::server::storage::value_or_empty(*persisted, "raw_file_id") == raw_file.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "parsed_record_id") ==
           first_record.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "device_code") == "device-a");
    assert(labbridge::server::storage::value_or_empty(*persisted, "payload_json")
               .find("temperature") != std::string::npos);
    assert(labbridge::server::storage::value_or_empty(*persisted, "status") == "succeeded");

    return 0;
}
