#include "labbridge/core/version.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/alert_repository.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/application/query_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/postgres/storage_mapping.h"
#include "labbridge/server/application/task_run_service.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

const labbridge::server::RawFileRecord* find_raw_file(
    const std::vector<labbridge::server::RawFileRecord>& raw_files,
    const std::string& raw_file_id) {
    for (const auto& raw_file : raw_files) {
        if (raw_file.id == raw_file_id) {
            return &raw_file;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL archive trace smoke test\n";
        return 77;
    }

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::PostgresResultRepository result_repository{session};
    labbridge::server::PostgresQcRepository qc_repository{session};
    labbridge::server::PostgresAlertRepository alert_repository{session};

    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::ConfigService config_service{node_repository, config_repository};
    labbridge::server::TaskRunService task_run_service{config_repository, task_run_repository};
    labbridge::server::ResultService result_service{task_run_repository, result_repository};
    labbridge::server::ControlPlaneQueryService query_service{
        node_repository,
        config_repository,
        task_run_repository,
        result_repository,
        qc_repository,
        alert_repository};

    const std::string node_code = "lab-node-real-archive-015";
    const std::string other_node_code = "lab-node-real-archive-015-other";

    assert(node_service.register_node({node_code, "real-archive-node", labbridge::core::kVersion}).ok);
    assert(node_service.register_node({other_node_code, "real-archive-other-node", labbridge::core::kVersion}).ok);
    assert(node_service.accept_heartbeat({
               node_code,
               labbridge::core::kVersion,
               "2026-06-01 10:00:00+08",
           }).ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase15 local csv dir",
        R"({"path":"tests/fixtures/agent","pattern":"*.csv"})",
        true,
    });
    assert(data_source.status.ok);
    assert(!data_source.id.empty());

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase15 archive trace csv",
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
        "2026-06-01 10:01:00+08",
        "manual",
    });
    assert(started.status.ok);
    assert(!started.id.empty());

    const auto archived_raw_file = result_service.record_raw_file({
        started.id,
        node_code,
        "sample_observation.csv",
        "phase15-real-hash-archived",
        "/archive/phase15/sample_observation.csv",
        128,
        "2026-06-01 09:59:00+08",
        "archived",
    });
    assert(archived_raw_file.status.ok);
    assert(!archived_raw_file.id.empty());

    const auto failed_raw_file = result_service.record_raw_file({
        started.id,
        node_code,
        "broken_observation.csv",
        "phase15-real-hash-failed",
        "/archive/phase15/broken_observation.csv",
        64,
        "2026-06-01 09:58:00+08",
        "archive_failed",
    });
    assert(failed_raw_file.status.ok);
    assert(!failed_raw_file.id.empty());

    const auto parsed_record = result_service.record_parsed_record({
        started.id,
        archived_raw_file.id,
        {
            "station-a",
            "device-a",
            "2026-06-01 10:00:00+08",
            R"({"temperature":21.5,"humidity":62})",
        },
        "parsed",
    });
    assert(parsed_record.status.ok);
    assert(!parsed_record.id.empty());

    const auto finish_status = task_run_service.finish({
        started.id,
        labbridge::core::TaskRunStatus::Failed,
        "2026-06-01 10:03:00+08",
        2,
        1,
        1,
        "archive failed for one file",
    });
    assert(finish_status.ok);

    const auto detail = query_service.find_task_run_detail(node_code, started.id);
    assert(detail.status.ok);
    assert(detail.task_run.has_value());
    assert(detail.task_run->id == started.id);
    assert(detail.raw_files.size() == 2);

    const auto* archived_file = find_raw_file(detail.raw_files, archived_raw_file.id);
    assert(archived_file != nullptr);
    assert(archived_file->storage_path == "/archive/phase15/sample_observation.csv");
    assert(archived_file->ingest_status == "archived");

    const auto* failed_file = find_raw_file(detail.raw_files, failed_raw_file.id);
    assert(failed_file != nullptr);
    assert(failed_file->ingest_status == "archive_failed");

    const auto wrong_node_detail = query_service.find_task_run_detail(other_node_code, started.id);
    assert(!wrong_node_detail.status.ok);

    const auto persisted = session.query_one(
        "SELECT n.node_code, tr.id::text AS task_run_id, rf.id::text AS raw_file_id, "
        "rf.storage_path, rf.ingest_status, COALESCE(pr.id::text, '') AS parsed_record_id, "
        "COALESCE(pr.raw_file_id::text, '') AS parsed_raw_file_id, tr.status "
        "FROM raw_files rf "
        "JOIN task_runs tr ON tr.id = rf.task_run_id "
        "JOIN nodes n ON n.id = rf.node_id "
        "LEFT JOIN parsed_records pr ON pr.raw_file_id = rf.id "
        "WHERE n.node_code = $1 AND tr.id = $2::bigint AND rf.id = $3::bigint "
        "LIMIT 1",
        {node_code, started.id, archived_raw_file.id});

    assert(persisted.has_value());
    assert(labbridge::server::storage::value_or_empty(*persisted, "node_code") == node_code);
    assert(labbridge::server::storage::value_or_empty(*persisted, "task_run_id") == started.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "raw_file_id") == archived_raw_file.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "storage_path") == "/archive/phase15/sample_observation.csv");
    assert(labbridge::server::storage::value_or_empty(*persisted, "ingest_status") == "archived");
    assert(labbridge::server::storage::value_or_empty(*persisted, "parsed_record_id") == parsed_record.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "parsed_raw_file_id") == archived_raw_file.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "status") == "failed");

    return 0;
}
