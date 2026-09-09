#include "labbridge/core/version.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/application/qc_service.h"
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
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL QC flow smoke test\n";
        return 77;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::PostgresResultRepository result_repository{session};
    labbridge::server::PostgresQcRepository qc_repository{session};
    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{config_repository};
    labbridge::server::TaskRunService task_run_service{config_repository, task_run_repository};
    labbridge::server::ResultService result_service{task_run_repository, result_repository};
    labbridge::server::QcService qc_service{qc_repository};

    const std::string node_code = "lab-node-real-qc-011";

    const auto register_status = node_service.register_node({
        node_code,
        "real-qc-flow-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);

    const auto heartbeat_status = node_service.accept_heartbeat({
        node_code,
        labbridge::core::kVersion,
        "2026-05-27 10:00:00+08",
    });
    assert(heartbeat_status.ok);

    const auto data_source =
        labbridge::server::test_support::create_local_csv_data_source(
            config_repository, node_code, "phase11 local csv dir",
            R"({"path":"tests/fixtures/agent","pattern":"*.csv"})");
    assert(!data_source.empty());

    const auto task = labbridge::server::test_support::create_csv_task(
        config_repository, node_code, data_source,
        "phase11 collect local csv");
    assert(!task.empty());

    const auto started = task_run_service.start({
        node_code,
        task,
        "2026-05-27T02:01:00Z",
        "manual",
    });
    assert(started.status.ok);
    assert(!started.id.empty());

    const auto raw_file = result_service.record_raw_file({
        started.id,
        node_code,
        "sample_observation.csv",
        "phase11-hash-001",
        "/archive/phase11/sample_observation.csv",
        128,
        "2026-05-27T01:59:00Z",
        "collected",
    });
    assert(raw_file.status.ok);
    assert(!raw_file.id.empty());

    const auto stored_raw_file = result_repository.find_raw_file(raw_file.id);
    assert(stored_raw_file.has_value());

    const auto parsed_record = result_service.record_parsed_record(
        {
            started.id,
            raw_file.id,
            {
                "station-a",
                "device-a",
                "2026-05-27T02:00:00Z",
                R"({"temperature":21.5,"humidity":62})",
            },
            "parsed",
        },
        *stored_raw_file);
    assert(parsed_record.status.ok);
    assert(!parsed_record.id.empty());

    const auto required_rule = qc_service.create_rule({
        "phase11 required fields",
        "required_fields",
        R"({"required":["station_code","device_code","record_time"]})",
        true,
    });
    assert(required_rule.status.ok);
    assert(!required_rule.id.empty());

    const auto timestamp_rule = qc_service.create_rule({
        "phase11 timestamp format",
        "basic_timestamp_format",
        R"({"format":"yyyy-mm-dd hh:mm:ss"})",
        true,
    });
    assert(timestamp_rule.status.ok);
    assert(!timestamp_rule.id.empty());

    const auto required_result = qc_service.record_result({
        parsed_record.id,
        required_rule.id,
        "pass",
        "passed",
        "required fields are present",
    });
    assert(required_result.status.ok);
    assert(!required_result.record.id.empty());

    const auto timestamp_result = qc_service.record_result({
        parsed_record.id,
        timestamp_rule.id,
        "pass",
        "passed",
        "record_time format is valid",
    });
    assert(timestamp_result.status.ok);
    assert(!timestamp_result.record.id.empty());

    const auto persisted_required =
        qc_repository.find_result(required_result.record.id);
    assert(persisted_required.has_value());
    assert(persisted_required->qc_rule_id == required_rule.id);
    assert(persisted_required->level == "pass");
    assert(persisted_required->result == "passed");

    const auto persisted_timestamp =
        qc_repository.find_result(timestamp_result.record.id);
    assert(persisted_timestamp.has_value());
    assert(persisted_timestamp->qc_rule_id == timestamp_rule.id);
    assert(persisted_timestamp->level == "pass");
    assert(persisted_timestamp->result == "passed");

    const auto finish_status = task_run_service.finish({
        started.id,
        labbridge::core::TaskRunStatus::Succeeded,
        "2026-05-27T02:03:00Z",
        1,
        1,
        0,
        "",
    });
    assert(finish_status.ok);

    const auto persisted = session.query_one(
        "SELECT n.node_code, pr.id::text AS parsed_record_id, qr.id::text AS qc_result_id, "
        "rule.id::text AS qc_rule_id, rule.rule_type, qr.level, qr.result, tr.status "
        "FROM qc_results qr "
        "JOIN qc_rules rule ON rule.id = qr.qc_rule_id "
        "JOIN parsed_records pr ON pr.id = qr.parsed_record_id "
        "JOIN task_runs tr ON tr.id = pr.task_run_id "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE n.node_code = $1 AND pr.id = $2::bigint AND qr.id = $3::bigint "
        "LIMIT 1",
        {node_code, parsed_record.id, required_result.record.id});

    assert(persisted.has_value());
    assert(labbridge::server::storage::value_or_empty(*persisted, "node_code") == node_code);
    assert(labbridge::server::storage::value_or_empty(*persisted, "parsed_record_id") ==
           parsed_record.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "qc_result_id") ==
           required_result.record.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "qc_rule_id") ==
           required_rule.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "rule_type") ==
           "required_fields");
    assert(labbridge::server::storage::value_or_empty(*persisted, "level") == "pass");
    assert(labbridge::server::storage::value_or_empty(*persisted, "result") == "passed");
    assert(labbridge::server::storage::value_or_empty(*persisted, "status") == "succeeded");

    return 0;
}
