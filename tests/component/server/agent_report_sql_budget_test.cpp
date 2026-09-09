// A4 专项：一次 2 记录×2 规则（同 raw file、同规则）的 report 请求，
// SQL 读次数必须收敛为常数（run 1 + 规则 1 + raw file 1），
// 写次数为每记录一次 + 每规则结果一次 + 告警 + 收尾。
#include "support/server/in_memory_repositories.h"
#include "support/server/test_config_seed.h"
#include "labbridge/core/version.h"
#include "labbridge/server/application/agent_report_service.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/application/task_run_service.h"
#include "labbridge/server/postgres/alert_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "support/server/recording_sql_session.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using labbridge::server::SqlRow;
using labbridge::server::test_support::RecordingSqlSession;

bool sql_contains(const std::string& sql, const std::string& needle) {
    return sql.find(needle) != std::string::npos;
}

TEST(AgentReportSqlBudgetTest, ReportRequestUsesConstantReadsAndPerItemWrites) {
    RecordingSqlSession session;
    int next_id = 5000;
    labbridge::server::InMemoryNodeRepository node_repository;
    labbridge::server::InMemoryConfigRepository config_repository;
    labbridge::server::InMemoryTaskRunRepository in_memory_runs;
    labbridge::server::InMemoryAgentReportReceiptRepository receipt_repository;

    // run 元数据用 in-memory 仓储（find_run 不产生 SQL），
    // 结果/QC/告警/收尾走 Postgres 仓储 + RecordingSqlSession。
    labbridge::server::PostgresTaskRunRepository task_run_repository{session};
    labbridge::server::PostgresResultRepository result_repository{session};
    labbridge::server::PostgresQcRepository qc_repository{session};
    labbridge::server::PostgresAlertRepository alert_repository{session};

    labbridge::server::NodeService node_service{node_repository};
    labbridge::server::TaskRunService task_run_service{
        config_repository, in_memory_runs};
    labbridge::server::ResultService result_service{
        in_memory_runs, result_repository};
    labbridge::server::QcService qc_service{qc_repository};
    labbridge::server::AlertService alert_service{
        in_memory_runs, result_repository, qc_repository, alert_repository};
    labbridge::server::AgentReportService agent_report_service{
        task_run_service,
        result_service,
        qc_service,
        alert_service,
        receipt_repository};

    const std::string node_code = "budget-node";
    const std::string task_id = labbridge::server::test_support::create_csv_task(
        config_repository, node_code, "1", "budget task");
    const auto started = task_run_service.start({
        node_code,
        task_id,
        "2026-08-08T00:00:00Z",
        "budget",
    });
    ASSERT_TRUE(started.status.ok);

    // 规则与 raw file 直接经仓储 mock 返回，不经过上报写入路径。
    SqlRow rule_row{
        {"id", "21"},
        {"name", "required"},
        {"rule_type", "required_fields"},
        {"rule_config_json", "{}"},
        {"enabled", "true"},
    };
    SqlRow raw_file_row{
        {"id", "101"},
        {"task_run_id", started.id},
        {"node_code", node_code},
        {"original_name", "sample.csv"},
        {"file_hash", ""},
        {"storage_path", "/archive/sample.csv"},
        {"size_bytes", "10"},
        {"source_mtime", ""},
        {"ingest_status", "collected"},
    };
    session.on_query_one = [&](const std::string& sql,
                               const auto&) -> std::optional<SqlRow> {
        if (sql_contains(sql, "FROM qc_rules")) {
            return rule_row;
        }
        if (sql_contains(sql, "FROM raw_files rf")) {
            return raw_file_row;
        }
        if (sql_contains(sql, "INSERT INTO parsed_records") ||
            sql_contains(sql, "INSERT INTO qc_results") ||
            sql_contains(sql, "INSERT INTO alerts")) {
            return SqlRow{{"id", std::to_string(++next_id)}};
        }
        return std::nullopt;
    };

    const auto report = agent_report_service.accept_task_run_report({
        started.id,
        node_code,
        "budget-report",
        labbridge::core::TaskRunStatus::Failed,
        "2026-08-08T00:01:00Z",
        2,
        0,
        2,
        "qc failed",
        {
            {"101",
             {"station-a", "device-a", "2026-08-08T00:00:00Z", "{}"},
             "parsed",
             {
                 {"21", "pass", "passed", "ok"},
                 {"21", "failed", "failed", "bad"},
             }},
            {"101",
             {"station-a", "device-b", "2026-08-08T00:00:01Z", "{}"},
             "parsed",
             {
                 {"21", "pass", "passed", "ok"},
                 {"21", "failed", "failed", "bad"},
             }},
        },
    });
    ASSERT_TRUE(report.status.ok) << report.status.message;
    ASSERT_EQ(report.parsed_record_ids.size(), 2U);
    ASSERT_EQ(report.qc_result_ids.size(), 4U);
    ASSERT_EQ(report.alert_ids.size(), 2U);

    int reads = 0;
    int writes = 0;
    for (const auto& call : session.query_one_calls) {
        if (sql_contains(call.sql, "INSERT INTO")) {
            ++writes;
        } else {
            ++reads;
        }
    }
    writes += static_cast<int>(session.executions.size());

    // 读：规则 1 + raw file 1（run 元数据走 in-memory 仓储不产生 SQL；
    // 同 raw file 与同规则在本请求内均只查一次）。
    EXPECT_EQ(reads, 2);
    // 写：parsed 2 + qc 4 + alert 2（收尾走 in-memory 仓储不产生 SQL）。
    EXPECT_EQ(writes, 8);
}

}  // namespace
