#include "labbridge/core/version.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/alert_repository.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/postgres/storage_mapping.h"
#include "labbridge/server/application/task_run_service.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL alert flow smoke test\n";
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
    labbridge::server::QcService qc_service{result_repository, qc_repository};
    labbridge::server::AlertService alert_service{
        task_run_repository,
        result_repository,
        qc_repository,
        alert_repository};

    const std::string node_code = "lab-node-real-alert-013";

    const auto register_status = node_service.register_node({
        node_code,
        "real-alert-flow-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);

    const auto heartbeat_status = node_service.accept_heartbeat({
        node_code,
        labbridge::core::kVersion,
        "2026-05-28 10:00:00+08",
    });
    assert(heartbeat_status.ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase13 local csv dir",
        R"({"path":"tests/fixtures/agent","pattern":"*.csv"})",
        true,
    });
    assert(data_source.status.ok);
    assert(!data_source.id.empty());

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase13 collect local csv",
        "collect_parse_qc",
        "* * * * *",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);
    assert(!task.id.empty());

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-05-28 10:01:00+08",
        "manual",
    });
    assert(started.status.ok);
    assert(!started.id.empty());

    const auto raw_file = result_service.record_raw_file({
        started.id,
        node_code,
        "sample_observation.csv",
        "phase13-hash-001",
        "/archive/phase13/sample_observation.csv",
        128,
        "2026-05-28 09:59:00+08",
        "collected",
    });
    assert(raw_file.status.ok);
    assert(!raw_file.id.empty());

    const auto parsed_record = result_service.record_parsed_record({
        started.id,
        raw_file.id,
        {
            "station-a",
            "device-a",
            "2026-05-28 10:00:00+08",
            R"({"temperature":48.5,"humidity":62})",
        },
        "parsed",
    });
    assert(parsed_record.status.ok);
    assert(!parsed_record.id.empty());

    const auto range_rule = qc_service.create_rule({
        "phase13 temperature range",
        "range_check",
        R"({"field":"temperature","min":0,"max":40})",
        true,
    });
    assert(range_rule.status.ok);
    assert(!range_rule.id.empty());

    const auto pass_result = qc_service.record_result({
        parsed_record.id,
        range_rule.id,
        "pass",
        "passed",
        "humidity is in range",
    });
    assert(pass_result.status.ok);
    assert(!pass_result.id.empty());

    const auto pass_alert = alert_service.create_from_qc_result({pass_result.id});
    assert(!pass_alert.status.ok);

    const auto warning_result = qc_service.record_result({
        parsed_record.id,
        range_rule.id,
        "warning",
        "warning",
        "temperature is near upper limit",
    });
    assert(warning_result.status.ok);
    assert(!warning_result.id.empty());

    const auto warning_alert = alert_service.create_from_qc_result({warning_result.id});
    assert(warning_alert.status.ok);
    assert(!warning_alert.id.empty());

    const auto failed_result = qc_service.record_result({
        parsed_record.id,
        range_rule.id,
        "failed",
        "failed",
        "temperature is outside configured range",
    });
    assert(failed_result.status.ok);
    assert(!failed_result.id.empty());

    const auto failed_alert = alert_service.create_from_qc_result({failed_result.id});
    assert(failed_alert.status.ok);
    assert(!failed_alert.id.empty());

    const auto finish_status = task_run_service.finish({
        started.id,
        labbridge::core::TaskRunStatus::Failed,
        "2026-05-28 10:03:00+08",
        1,
        0,
        1,
        "qc failed",
    });
    assert(finish_status.ok);

    const auto run_alerts = alert_service.find_alerts_by_task_run(started.id);
    bool found_warning_alert = false;
    bool found_failed_alert = false;
    for (const auto& alert : run_alerts) {
        if (alert.id == warning_alert.id) {
            found_warning_alert = true;
            assert(alert.node_code == node_code);
            assert(alert.alert_type == "qc_result");
            assert(alert.severity == "warning");
            assert(alert.status == "open");
        }
        if (alert.id == failed_alert.id) {
            found_failed_alert = true;
            assert(alert.node_code == node_code);
            assert(alert.alert_type == "qc_result");
            assert(alert.severity == "failed");
            assert(alert.message == "temperature is outside configured range");
            assert(alert.status == "open");
        }
    }
    assert(found_warning_alert);
    assert(found_failed_alert);

    const auto node_alerts = alert_service.find_alerts_by_node(node_code);
    bool found_node_warning_alert = false;
    bool found_node_failed_alert = false;
    for (const auto& alert : node_alerts) {
        if (alert.id == warning_alert.id) {
            found_node_warning_alert = true;
        }
        if (alert.id == failed_alert.id) {
            found_node_failed_alert = true;
        }
    }
    assert(found_node_warning_alert);
    assert(found_node_failed_alert);

    const auto persisted = session.query_one(
        "SELECT n.node_code, a.id::text AS alert_id, a.task_run_id::text AS task_run_id, "
        "a.alert_type, a.severity, a.message, a.status, tr.status AS task_run_status "
        "FROM alerts a "
        "JOIN nodes n ON n.id = a.node_id "
        "JOIN task_runs tr ON tr.id = a.task_run_id "
        "WHERE a.id = $1::bigint AND n.node_code = $2 "
        "LIMIT 1",
        {failed_alert.id, node_code});

    assert(persisted.has_value());
    assert(labbridge::server::storage::value_or_empty(*persisted, "node_code") == node_code);
    assert(labbridge::server::storage::value_or_empty(*persisted, "alert_id") ==
           failed_alert.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "task_run_id") == started.id);
    assert(labbridge::server::storage::value_or_empty(*persisted, "alert_type") == "qc_result");
    assert(labbridge::server::storage::value_or_empty(*persisted, "severity") == "failed");
    assert(labbridge::server::storage::value_or_empty(*persisted, "message") ==
           "temperature is outside configured range");
    assert(labbridge::server::storage::value_or_empty(*persisted, "status") == "open");
    assert(labbridge::server::storage::value_or_empty(*persisted, "task_run_status") == "failed");

    return 0;
}
