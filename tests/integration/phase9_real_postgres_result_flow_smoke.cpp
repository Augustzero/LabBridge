#include "labbridge/core/version.h"
#include "labbridge/server/config_service.h"
#include "labbridge/server/libpq_sql_session.h"
#include "labbridge/server/node_service.h"
#include "labbridge/server/postgres_config_repository.h"
#include "labbridge/server/postgres_node_repository.h"
#include "labbridge/server/postgres_result_repository.h"
#include "labbridge/server/postgres_task_run_repository.h"
#include "labbridge/server/result_service.h"
#include "labbridge/server/storage_mapping.h"
#include "labbridge/server/task_run_service.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL result flow smoke test\n";
        return 0;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::PostgresResultRepository result_repository{session};
    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{node_repository, config_repository};
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
        "2026-05-26 10:00:00+08",
    });
    assert(heartbeat_status.ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase9 local csv dir",
        R"({"path":"scripts/mock_data","pattern":"*.csv"})",
        true,
    });
    assert(data_source.status.ok);
    assert(!data_source.id.empty());

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase9 collect local csv",
        "collect_parse_qc",
        "manual",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);
    assert(!task.id.empty());

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-05-26 10:01:00+08",
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
        "2026-05-26 09:59:00+08",
        "collected",
    });
    assert(raw_file.status.ok);
    assert(!raw_file.id.empty());

    const auto stored_raw_file = result_repository.find_raw_file(raw_file.id);
    assert(stored_raw_file.has_value());
    assert(stored_raw_file->task_run_id == started.id);
    assert(stored_raw_file->node_code == node_code);
    assert(stored_raw_file->original_name == "sample_observation.csv");

    const auto first_record = result_service.record_parsed_record({
        started.id,
        raw_file.id,
        {
            "station-a",
            "device-a",
            "2026-05-26 10:00:00+08",
            R"({"temperature":21.5,"humidity":62})",
        },
        "parsed",
    });
    assert(first_record.status.ok);
    assert(!first_record.id.empty());

    const auto second_record = result_service.record_parsed_record({
        started.id,
        raw_file.id,
        {
            "station-a",
            "device-b",
            "2026-05-26 10:05:00+08",
            R"({"temperature":22.1,"humidity":60})",
        },
        "parsed",
    });
    assert(second_record.status.ok);
    assert(!second_record.id.empty());

    const auto parsed_records = result_service.find_parsed_records(started.id);
    assert(parsed_records.size() >= 2);

    bool found_first_record = false;
    bool found_second_record = false;
    for (const auto& parsed_record : parsed_records) {
        if (parsed_record.id == first_record.id) {
            found_first_record = true;
            assert(parsed_record.raw_file_id == raw_file.id);
            assert(parsed_record.record.device_code == "device-a");
            assert(parsed_record.record.payload_json.find("temperature") != std::string::npos);
        }
        if (parsed_record.id == second_record.id) {
            found_second_record = true;
            assert(parsed_record.raw_file_id == raw_file.id);
            assert(parsed_record.record.device_code == "device-b");
            assert(parsed_record.record.payload_json.find("humidity") != std::string::npos);
        }
    }
    assert(found_first_record);
    assert(found_second_record);

    const auto finish_status = task_run_service.finish({
        started.id,
        labbridge::core::TaskRunStatus::Succeeded,
        "2026-05-26 10:06:00+08",
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
