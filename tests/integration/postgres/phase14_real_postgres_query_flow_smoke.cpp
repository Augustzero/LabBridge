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
#include "labbridge/server/application/query_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/application/task_run_service.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool contains_task(const std::vector<labbridge::server::TaskRecord>& tasks,
                   const std::string& task_id) {
    for (const auto& task : tasks) {
        if (task.id == task_id) {
            return true;
        }
    }
    return false;
}

bool contains_qc_result(const std::vector<labbridge::server::QcResultRecord>& results,
                        const std::string& qc_result_id) {
    for (const auto& result : results) {
        if (result.id == qc_result_id) {
            return true;
        }
    }
    return false;
}

bool contains_alert(const std::vector<labbridge::server::AlertRecord>& alerts,
                    const std::string& alert_id) {
    for (const auto& alert : alerts) {
        if (alert.id == alert_id) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL query flow smoke test\n";
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
    labbridge::server::ControlPlaneQueryService query_service{
        node_repository,
        config_repository,
        task_run_repository,
        result_repository,
        qc_repository,
        alert_repository};

    const std::string node_code = "lab-node-real-query-014";
    const std::string other_node_code = "lab-node-real-query-014-other";

    assert(node_service.register_node({
               node_code,
               "real-query-flow-node",
               labbridge::core::kVersion,
           }).ok);
    assert(node_service.register_node({
               other_node_code,
               "real-query-flow-other-node",
               labbridge::core::kVersion,
           }).ok);
    assert(node_service.accept_heartbeat({
               node_code,
               labbridge::core::kVersion,
               "2026-05-29 10:00:00+08",
           }).ok);

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase14 local csv dir",
        R"({"path":"tests/fixtures/agent","pattern":"*.csv"})",
        true,
    });
    assert(data_source.status.ok);
    assert(!data_source.id.empty());

    const auto task = config_service.create_task({
        node_code,
        data_source.id,
        "phase14 collect local csv",
        "collect_parse_qc",
        "manual",
        "csv_observation",
        "basic",
        true,
    });
    assert(task.status.ok);
    assert(!task.id.empty());

    const auto disabled_task = config_service.create_task({
        node_code,
        data_source.id,
        "phase14 disabled local csv",
        "collect_parse_qc",
        "manual",
        "csv_observation",
        "basic",
        false,
    });
    assert(disabled_task.status.ok);
    assert(!disabled_task.id.empty());

    const auto started = task_run_service.start({
        node_code,
        task.id,
        "2026-05-29 10:01:00+08",
        "manual",
    });
    assert(started.status.ok);
    assert(!started.id.empty());

    const auto raw_file = result_service.record_raw_file({
        started.id,
        node_code,
        "sample_observation.csv",
        "phase14-hash-001",
        "/archive/phase14/sample_observation.csv",
        128,
        "2026-05-29 09:59:00+08",
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
            "2026-05-29 10:00:00+08",
            R"({"temperature":48.5,"humidity":62})",
        },
        "parsed",
    });
    assert(parsed_record.status.ok);
    assert(!parsed_record.id.empty());

    const auto range_rule = qc_service.create_rule({
        "phase14 temperature range",
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
        "2026-05-29 10:03:00+08",
        1,
        0,
        1,
        "qc failed",
    });
    assert(finish_status.ok);

    const auto node_overview = query_service.find_node_overview(node_code);
    assert(node_overview.status.ok);
    assert(node_overview.node.has_value());
    assert(node_overview.node->info.node_code == node_code);
    assert(node_overview.node->status == labbridge::core::NodeStatus::Online);
    assert(contains_task(node_overview.enabled_tasks, task.id));
    assert(!contains_task(node_overview.enabled_tasks, disabled_task.id));
    assert(contains_alert(node_overview.alerts, failed_alert.id));

    const auto detail = query_service.find_task_run_detail(node_code, started.id);
    assert(detail.status.ok);
    assert(detail.task_run.has_value());
    assert(detail.task_run->id == started.id);
    assert(detail.task_run->status == labbridge::core::TaskRunStatus::Failed);
    assert(contains_qc_result(detail.qc_results, pass_result.id));
    assert(contains_qc_result(detail.qc_results, failed_result.id));
    assert(contains_alert(detail.alerts, failed_alert.id));

    const auto wrong_node_detail = query_service.find_task_run_detail(other_node_code, started.id);
    assert(!wrong_node_detail.status.ok);

    return 0;
}
