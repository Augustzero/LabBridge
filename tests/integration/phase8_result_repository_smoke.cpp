#include "labbridge/server/postgres_result_repository.h"
#include "labbridge/server/result_service.h"
#include "labbridge/server/task_run_repository.h"

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
    }

    std::optional<labbridge::server::SqlRow> query_one(
        const std::string& sql,
        const labbridge::server::SqlParams& params) override {
        queried.push_back({sql, params});

        if (sql.find("INSERT INTO raw_files") != std::string::npos) {
            raw_file_row["id"] = "801";
            raw_file_row["task_run_id"] = params[0];
            raw_file_row["node_code"] = params[1];
            raw_file_row["original_name"] = params[2];
            raw_file_row["file_hash"] = params[3];
            raw_file_row["storage_path"] = params[4];
            raw_file_row["size_bytes"] = params[5];
            raw_file_row["source_mtime"] = params[6];
            raw_file_row["ingest_status"] = params[7];
            return labbridge::server::SqlRow{{"id", "801"}};
        }

        if (sql.find("INSERT INTO parsed_records") != std::string::npos) {
            parsed_record_row["id"] = "901";
            parsed_record_row["raw_file_id"] = params[0];
            parsed_record_row["task_run_id"] = params[1];
            parsed_record_row["station_code"] = params[2];
            parsed_record_row["device_code"] = params[3];
            parsed_record_row["record_time"] = params[4];
            parsed_record_row["payload_json"] = params[5];
            parsed_record_row["parse_status"] = params[6];
            return labbridge::server::SqlRow{{"id", "901"}};
        }

        if (sql.find("FROM raw_files rf") != std::string::npos) {
            if (raw_file_row.empty() || raw_file_row["id"] != params[0]) {
                return std::nullopt;
            }
            return raw_file_row;
        }

        return std::nullopt;
    }

    std::vector<labbridge::server::SqlRow> query_all(
        const std::string& sql,
        const labbridge::server::SqlParams& params) override {
        queried.push_back({sql, params});
        if (sql.find("FROM parsed_records") == std::string::npos || parsed_record_row.empty()) {
            return {};
        }
        if (parsed_record_row["task_run_id"] != params[0]) {
            return {};
        }
        return {parsed_record_row};
    }

    std::vector<RecordedStatement> executed;
    std::vector<RecordedStatement> queried;
    labbridge::server::SqlRow raw_file_row;
    labbridge::server::SqlRow parsed_record_row;
};

}  // namespace

int main() {
    RecordingSqlSession session;
    labbridge::server::InMemoryTaskRunRepository task_run_repository;
    labbridge::server::PostgresResultRepository result_repository(session);
    labbridge::server::ResultService result_service(task_run_repository, result_repository);

    labbridge::server::TaskRunRecord task_run;
    task_run.task_id = "401";
    task_run.node_code = "lab-node-result-008";
    task_run.status = labbridge::core::TaskRunStatus::Running;
    task_run.started_at = "2026-05-26 10:00:00+08";
    const auto task_run_id = task_run_repository.create(std::move(task_run));

    const auto wrong_node_raw_file = result_service.record_raw_file({
        task_run_id,
        "other-node",
        "sample_observation.csv",
        "hash-001",
        "/archive/sample_observation.csv",
        128,
        "2026-05-26 09:59:00+08",
        "collected",
    });
    assert(!wrong_node_raw_file.status.ok);

    const auto raw_file = result_service.record_raw_file({
        task_run_id,
        "lab-node-result-008",
        "sample_observation.csv",
        "hash-001",
        "/archive/sample_observation.csv",
        128,
        "2026-05-26 09:59:00+08",
        "collected",
    });
    assert(raw_file.status.ok);
    assert(raw_file.id == "801");
    assert(session.queried.back().sql.find("INSERT INTO raw_files") != std::string::npos);
    assert(session.queried.back().params[1] == "lab-node-result-008");

    const auto missing_raw_file_record = result_service.record_parsed_record({
        task_run_id,
        "missing-raw-file",
        {
            "station-a",
            "device-a",
            "2026-05-26 10:00:00+08",
            R"({"temperature":21.5})",
        },
        "parsed",
    });
    assert(!missing_raw_file_record.status.ok);

    const auto parsed_record = result_service.record_parsed_record({
        task_run_id,
        raw_file.id,
        {
            "station-a",
            "device-a",
            "2026-05-26 10:00:00+08",
            R"({"temperature":21.5})",
        },
        "parsed",
    });
    assert(parsed_record.status.ok);
    assert(parsed_record.id == "901");
    assert(session.queried.back().sql.find("INSERT INTO parsed_records") != std::string::npos);
    assert(session.queried.back().params[5] == R"({"temperature":21.5})");

    const auto parsed_records = result_service.find_parsed_records(task_run_id);
    assert(parsed_records.size() == 1);
    assert(parsed_records.front().raw_file_id == raw_file.id);
    assert(parsed_records.front().record.station_code == "station-a");
    assert(parsed_records.front().record.payload_json == R"({"temperature":21.5})");

    return 0;
}
