#include "support/server/in_memory_repositories.h"
#include "labbridge/core/version.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/application/query_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/application/task_run_service.h"

#include <cassert>
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

const labbridge::server::ParsedRecordRecord* find_parsed_record(
    const std::vector<labbridge::server::ParsedRecordRecord>& parsed_records,
    const std::string& parsed_record_id) {
    for (const auto& parsed_record : parsed_records) {
        if (parsed_record.id == parsed_record_id) {
            return &parsed_record;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    labbridge::server::InMemoryNodeRepository node_repository;
    labbridge::server::InMemoryConfigRepository config_repository;
    labbridge::server::InMemoryTaskRunRepository task_run_repository;
    labbridge::server::InMemoryResultRepository result_repository;
    labbridge::server::InMemoryQcRepository qc_repository;
    labbridge::server::InMemoryAlertRepository alert_repository;

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

    const std::string node_code = "lab-node-archive-015";
    const std::string other_node_code = "lab-node-archive-015-other";

    assert(node_service.register_node({node_code, "archive-node", labbridge::core::kVersion}).ok);
    assert(node_service.register_node({other_node_code, "archive-other-node", labbridge::core::kVersion}).ok);
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

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-06-01 10:01:00+08",
        "manual",
    });
    assert(started.status.ok);

    const auto archived_raw_file = result_service.record_raw_file({
        started.id,
        node_code,
        "sample_observation.csv",
        "phase15-hash-archived",
        "/archive/phase15/sample_observation.csv",
        128,
        "2026-06-01 09:59:00+08",
        "archived",
    });
    assert(archived_raw_file.status.ok);

    const auto failed_raw_file = result_service.record_raw_file({
        started.id,
        node_code,
        "broken_observation.csv",
        "phase15-hash-failed",
        "/archive/phase15/broken_observation.csv",
        64,
        "2026-06-01 09:58:00+08",
        "archive_failed",
    });
    assert(failed_raw_file.status.ok);

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
    assert(archived_file->node_code == node_code);

    const auto* failed_file = find_raw_file(detail.raw_files, failed_raw_file.id);
    assert(failed_file != nullptr);
    assert(failed_file->storage_path == "/archive/phase15/broken_observation.csv");
    assert(failed_file->ingest_status == "archive_failed");

    const auto* parsed = find_parsed_record(detail.parsed_records, parsed_record.id);
    assert(parsed != nullptr);
    assert(parsed->raw_file_id == archived_raw_file.id);
    assert(parsed->task_run_id == started.id);
    assert(parsed->record.payload_json.find("temperature") != std::string::npos);

    const auto wrong_node_detail = query_service.find_task_run_detail(other_node_code, started.id);
    assert(!wrong_node_detail.status.ok);

    return 0;
}
