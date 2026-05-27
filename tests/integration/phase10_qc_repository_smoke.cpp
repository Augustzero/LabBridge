#include "labbridge/server/postgres_qc_repository.h"
#include "labbridge/server/postgres_result_repository.h"
#include "labbridge/server/qc_service.h"
#include "labbridge/server/result_service.h"
#include "labbridge/server/task_run_repository.h"

#include <cassert>
#include <optional>
#include <string>
#include <unordered_map>
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
            raw_file_row["id"] = "1001";
            raw_file_row["task_run_id"] = params[0];
            raw_file_row["node_code"] = params[1];
            raw_file_row["original_name"] = params[2];
            raw_file_row["file_hash"] = params[3];
            raw_file_row["storage_path"] = params[4];
            raw_file_row["size_bytes"] = params[5];
            raw_file_row["source_mtime"] = params[6];
            raw_file_row["ingest_status"] = params[7];
            return labbridge::server::SqlRow{{"id", "1001"}};
        }

        if (sql.find("INSERT INTO parsed_records") != std::string::npos) {
            parsed_record_row["id"] = "1101";
            parsed_record_row["raw_file_id"] = params[0];
            parsed_record_row["task_run_id"] = params[1];
            parsed_record_row["station_code"] = params[2];
            parsed_record_row["device_code"] = params[3];
            parsed_record_row["record_time"] = params[4];
            parsed_record_row["payload_json"] = params[5];
            parsed_record_row["parse_status"] = params[6];
            return labbridge::server::SqlRow{{"id", "1101"}};
        }

        if (sql.find("FROM raw_files rf") != std::string::npos) {
            if (raw_file_row.empty() || raw_file_row["id"] != params[0]) {
                return std::nullopt;
            }
            return raw_file_row;
        }

        if (sql.find("FROM parsed_records pr") != std::string::npos &&
            sql.find("WHERE pr.id") != std::string::npos) {
            if (parsed_record_row.empty() || parsed_record_row["id"] != params[0]) {
                return std::nullopt;
            }
            return parsed_record_row;
        }

        if (sql.find("INSERT INTO qc_rules") != std::string::npos) {
            last_rule_id = params[3] == "true" ? "1201" : "1202";
            qc_rule_rows[last_rule_id] = {
                {"id", last_rule_id},
                {"name", params[0]},
                {"rule_type", params[1]},
                {"rule_config_json", params[2]},
                {"enabled", params[3]},
            };
            return labbridge::server::SqlRow{{"id", last_rule_id}};
        }

        if (sql.find("FROM qc_rules") != std::string::npos) {
            const auto iter = qc_rule_rows.find(params[0]);
            if (iter == qc_rule_rows.end()) {
                return std::nullopt;
            }
            return iter->second;
        }

        if (sql.find("INSERT INTO qc_results") != std::string::npos) {
            qc_result_row["id"] = "1301";
            qc_result_row["parsed_record_id"] = params[0];
            qc_result_row["qc_rule_id"] = params[1];
            qc_result_row["level"] = params[2];
            qc_result_row["result"] = params[3];
            qc_result_row["message"] = params[4];
            return labbridge::server::SqlRow{{"id", "1301"}};
        }

        return std::nullopt;
    }

    std::vector<labbridge::server::SqlRow> query_all(
        const std::string& sql,
        const labbridge::server::SqlParams& params) override {
        queried.push_back({sql, params});

        if (sql.find("FROM qc_results") != std::string::npos) {
            if (qc_result_row.empty() || qc_result_row["parsed_record_id"] != params[0]) {
                return {};
            }
            return {qc_result_row};
        }

        if (sql.find("FROM parsed_records") != std::string::npos) {
            if (parsed_record_row.empty() || parsed_record_row["task_run_id"] != params[0]) {
                return {};
            }
            return {parsed_record_row};
        }

        return {};
    }

    std::vector<RecordedStatement> executed;
    std::vector<RecordedStatement> queried;
    std::string last_rule_id;
    labbridge::server::SqlRow raw_file_row;
    labbridge::server::SqlRow parsed_record_row;
    labbridge::server::SqlRow qc_result_row;
    std::unordered_map<std::string, labbridge::server::SqlRow> qc_rule_rows;
};

}  // namespace

int main() {
    RecordingSqlSession session;
    labbridge::server::InMemoryTaskRunRepository task_run_repository;
    labbridge::server::PostgresResultRepository result_repository(session);
    labbridge::server::PostgresQcRepository qc_repository(session);
    labbridge::server::ResultService result_service(task_run_repository, result_repository);
    labbridge::server::QcService qc_service(result_repository, qc_repository);

    labbridge::server::TaskRunRecord task_run;
    task_run.task_id = "401";
    task_run.node_code = "lab-node-qc-010";
    task_run.status = labbridge::core::TaskRunStatus::Running;
    task_run.started_at = "2026-05-27 10:00:00+08";
    const auto task_run_id = task_run_repository.create(std::move(task_run));

    const auto raw_file = result_service.record_raw_file({
        task_run_id,
        "lab-node-qc-010",
        "sample_observation.csv",
        "hash-010",
        "/archive/phase10/sample_observation.csv",
        128,
        "2026-05-27 09:59:00+08",
        "collected",
    });
    assert(raw_file.status.ok);

    const auto parsed_record = result_service.record_parsed_record({
        task_run_id,
        raw_file.id,
        {
            "station-a",
            "device-a",
            "2026-05-27 10:00:00+08",
            R"({"temperature":21.5})",
        },
        "parsed",
    });
    assert(parsed_record.status.ok);

    const auto disabled_rule = qc_service.create_rule({
        "disabled required fields",
        "required_fields",
        R"({"required":["station_code","device_code","record_time"]})",
        false,
    });
    assert(disabled_rule.status.ok);

    const auto disabled_result = qc_service.record_result({
        parsed_record.id,
        disabled_rule.id,
        "failed",
        "failed",
        "disabled rule should not record",
    });
    assert(!disabled_result.status.ok);

    const auto rule = qc_service.create_rule({
        "required fields",
        "required_fields",
        R"({"required":["station_code","device_code","record_time"]})",
        true,
    });
    assert(rule.status.ok);
    assert(rule.id == "1201");

    const auto missing_record_result = qc_service.record_result({
        "999999",
        rule.id,
        "failed",
        "failed",
        "missing parsed record",
    });
    assert(!missing_record_result.status.ok);

    const auto qc_result = qc_service.record_result({
        parsed_record.id,
        rule.id,
        "pass",
        "passed",
        "required fields are present",
    });
    assert(qc_result.status.ok);
    assert(qc_result.id == "1301");
    assert(session.queried.back().sql.find("INSERT INTO qc_results") != std::string::npos);
    assert(session.queried.back().params[2] == "pass");

    const auto results = qc_service.find_results(parsed_record.id);
    assert(results.size() == 1);
    assert(results.front().parsed_record_id == parsed_record.id);
    assert(results.front().qc_rule_id == rule.id);
    assert(results.front().level == "pass");
    assert(results.front().result == "passed");

    return 0;
}
