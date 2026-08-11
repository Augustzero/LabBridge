#include "support/server/in_memory_repositories.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/postgres/alert_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/repositories/task_run_repository.h"

#include <gtest/gtest.h>
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
            qc_rule_row = {
                {"id", "1201"},
                {"name", params[0]},
                {"rule_type", params[1]},
                {"rule_config_json", params[2]},
                {"enabled", params[3]},
            };
            return labbridge::server::SqlRow{{"id", "1201"}};
        }

        if (sql.find("FROM qc_rules") != std::string::npos) {
            if (qc_rule_row.empty() || qc_rule_row["id"] != params[0]) {
                return std::nullopt;
            }
            return qc_rule_row;
        }

        if (sql.find("INSERT INTO qc_results") != std::string::npos) {
            const auto id = std::to_string(next_qc_result_id_++);
            qc_result_rows[id] = {
                {"id", id},
                {"parsed_record_id", params[0]},
                {"qc_rule_id", params[1]},
                {"level", params[2]},
                {"result", params[3]},
                {"message", params[4]},
            };
            return labbridge::server::SqlRow{{"id", id}};
        }

        if (sql.find("FROM qc_results") != std::string::npos &&
            sql.find("WHERE id") != std::string::npos) {
            const auto iter = qc_result_rows.find(params[0]);
            if (iter == qc_result_rows.end()) {
                return std::nullopt;
            }
            return iter->second;
        }

        if (sql.find("INSERT INTO alerts") != std::string::npos) {
            const auto id = std::to_string(next_alert_id_++);
            alert_rows[id] = {
                {"id", id},
                {"node_code", params[0]},
                {"task_run_id", params[1]},
                {"alert_type", params[2]},
                {"severity", params[3]},
                {"message", params[4]},
                {"status", params[5]},
            };
            return labbridge::server::SqlRow{{"id", id}};
        }

        if (sql.find("FROM alerts a") != std::string::npos &&
            sql.find("WHERE a.id") != std::string::npos) {
            const auto iter = alert_rows.find(params[0]);
            if (iter == alert_rows.end()) {
                return std::nullopt;
            }
            return iter->second;
        }

        return std::nullopt;
    }

    std::vector<labbridge::server::SqlRow> query_all(
        const std::string& sql,
        const labbridge::server::SqlParams& params) override {
        queried.push_back({sql, params});

        std::vector<labbridge::server::SqlRow> rows;
        if (sql.find("FROM alerts a") != std::string::npos &&
            sql.find("WHERE n.node_code") != std::string::npos) {
            for (const auto& [id, alert] : alert_rows) {
                if (alert.at("node_code") == params[0]) {
                    rows.push_back(alert);
                }
            }
            return rows;
        }

        if (sql.find("FROM alerts a") != std::string::npos &&
            sql.find("WHERE a.task_run_id") != std::string::npos) {
            for (const auto& [id, alert] : alert_rows) {
                if (alert.at("task_run_id") == params[0]) {
                    rows.push_back(alert);
                }
            }
            return rows;
        }

        return {};
    }

    std::vector<RecordedStatement> executed;
    std::vector<RecordedStatement> queried;
    labbridge::server::SqlRow raw_file_row;
    labbridge::server::SqlRow parsed_record_row;
    labbridge::server::SqlRow qc_rule_row;
    std::unordered_map<std::string, labbridge::server::SqlRow> qc_result_rows;
    std::unordered_map<std::string, labbridge::server::SqlRow> alert_rows;

private:
    int next_qc_result_id_{1301};
    int next_alert_id_{1401};
};

}  // namespace

TEST(AlertServiceFlowTest, CreatesAlertsOnlyForNonPassingQc) {
    RecordingSqlSession session;
    labbridge::server::InMemoryTaskRunRepository task_run_repository;
    labbridge::server::PostgresResultRepository result_repository(session);
    labbridge::server::PostgresQcRepository qc_repository(session);
    labbridge::server::PostgresAlertRepository alert_repository(session);
    labbridge::server::ResultService result_service(task_run_repository, result_repository);
    labbridge::server::QcService qc_service(result_repository, qc_repository);
    labbridge::server::AlertService alert_service(
        task_run_repository,
        result_repository,
        qc_repository,
        alert_repository);

    labbridge::server::TaskRunRecord task_run;
    task_run.task_id = "401";
    task_run.node_code = "lab-node-alert-013";
    task_run.status = labbridge::core::TaskRunStatus::Running;
    task_run.started_at = "2026-05-28 10:00:00+08";
    const auto task_run_id = task_run_repository.create(std::move(task_run));

    const auto raw_file = result_service.record_raw_file({
        task_run_id,
        "lab-node-alert-013",
        "sample_observation.csv",
        "hash-013",
        "/archive/phase13/sample_observation.csv",
        128,
        "2026-05-28 09:59:00+08",
        "collected",
    });
    EXPECT_TRUE(raw_file.status.ok);

    const auto parsed_record = result_service.record_parsed_record({
        task_run_id,
        raw_file.id,
        {
            "station-a",
            "device-a",
            "2026-05-28 10:00:00+08",
            R"({"temperature":48.5})",
        },
        "parsed",
    });
    EXPECT_TRUE(parsed_record.status.ok);

    const auto rule = qc_service.create_rule({
        "temperature range",
        "range_check",
        R"({"field":"temperature","min":0,"max":40})",
        true,
    });
    EXPECT_TRUE(rule.status.ok);
    EXPECT_TRUE(rule.id == "1201");

    const auto pass_result = qc_service.record_result({
        parsed_record.id,
        rule.id,
        "pass",
        "passed",
        "temperature is in range",
    });
    EXPECT_TRUE(pass_result.status.ok);

    const auto pass_alert = alert_service.create_from_qc_result({pass_result.id});
    EXPECT_TRUE(!pass_alert.status.ok);

    const auto missing_alert = alert_service.create_from_qc_result({"999999"});
    EXPECT_TRUE(!missing_alert.status.ok);

    const auto warning_result = qc_service.record_result({
        parsed_record.id,
        rule.id,
        "warning",
        "warning",
        "temperature is near upper limit",
    });
    EXPECT_TRUE(warning_result.status.ok);

    const auto warning_alert = alert_service.create_from_qc_result({warning_result.id});
    EXPECT_TRUE(warning_alert.status.ok);
    EXPECT_TRUE(warning_alert.id == "1401");
    EXPECT_TRUE(session.queried.back().sql.find("INSERT INTO alerts") != std::string::npos);
    EXPECT_TRUE(session.queried.back().params[0] == "lab-node-alert-013");
    EXPECT_TRUE(session.queried.back().params[1] == task_run_id);
    EXPECT_TRUE(session.queried.back().params[2] == "qc_result");
    EXPECT_TRUE(session.queried.back().params[3] == "warning");
    EXPECT_TRUE(session.queried.back().params[4] == "temperature is near upper limit");
    EXPECT_TRUE(session.queried.back().params[5] == "open");

    const auto failed_result = qc_service.record_result({
        parsed_record.id,
        rule.id,
        "failed",
        "failed",
        "",
    });
    EXPECT_TRUE(failed_result.status.ok);

    const auto failed_alert = alert_service.create_from_qc_result({failed_result.id});
    EXPECT_TRUE(failed_alert.status.ok);
    EXPECT_TRUE(failed_alert.id == "1402");

    const auto persisted_failed_alert = alert_repository.find_by_id(failed_alert.id);
    EXPECT_TRUE(persisted_failed_alert.has_value());
    EXPECT_TRUE(persisted_failed_alert->severity == "failed");
    EXPECT_TRUE(persisted_failed_alert->message == "qc result 1303 is failed");
    EXPECT_TRUE(persisted_failed_alert->status == "open");

    const auto node_alerts = alert_service.find_alerts_by_node("lab-node-alert-013");
    EXPECT_TRUE(node_alerts.size() == 2);

    const auto run_alerts = alert_service.find_alerts_by_task_run(task_run_id);
    EXPECT_TRUE(run_alerts.size() == 2);

}
