#include "labbridge/server/postgres/task_run_repository.h"
#include "support/server/recording_sql_session.h"

#include <gtest/gtest.h>

namespace {

using labbridge::server::PostgresTaskRunRepository;
using labbridge::server::SqlRow;
using labbridge::server::test_support::RecordingSqlSession;

labbridge::server::TaskRunRecord sample_run() {
    labbridge::server::TaskRunRecord run;
    run.task_id = "401";
    run.node_code = "unit-node";
    run.status = labbridge::core::TaskRunStatus::Running;
    run.started_at = "2026-08-11T10:00:00Z";
    run.trigger_type = "scheduled";
    return run;
}

TEST(PostgresTaskRunRepositoryTest, CreateMapsRunToParameterizedSql) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "501"}}};
    };
    PostgresTaskRunRepository repository{session};

    const auto id = repository.create(sample_run());

    EXPECT_EQ(id, "501");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    const auto& statement = session.query_one_calls.front();
    EXPECT_NE(statement.sql.find("INSERT INTO task_runs"), std::string::npos);
    ASSERT_EQ(statement.params.size(), 9U);
    EXPECT_EQ(statement.params[0], "401");
    EXPECT_EQ(statement.params[1], "unit-node");
    EXPECT_EQ(statement.params[2], "running");
}

TEST(PostgresTaskRunRepositoryTest, FindByIdMapsRunSummary) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{
            {"id", "501"},
            {"task_id", "401"},
            {"node_code", "unit-node"},
            {"status", "failed"},
            {"started_at", "2026-08-11 10:00:00"},
            {"finished_at", "2026-08-11 10:01:00"},
            {"items_total", "3"},
            {"items_success", "2"},
            {"items_failed", "1"},
            {"error_summary", "one row failed"},
            {"trigger_type", "scheduled"},
            {"execution_key", "key-1"},
            {"scheduled_for", "2026-08-11T10:00:00Z"},
        }};
    };
    PostgresTaskRunRepository repository{session};

    const auto run = repository.find_by_id("501");

    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run->status, labbridge::core::TaskRunStatus::Failed);
    EXPECT_EQ(run->items_total, 3);
    EXPECT_EQ(run->items_success, 2);
    EXPECT_EQ(run->items_failed, 1);
    EXPECT_EQ(run->execution_key, "key-1");
}

TEST(PostgresTaskRunRepositoryTest, FinishMapsTerminalSummary) {
    RecordingSqlSession session;
    PostgresTaskRunRepository repository{session};
    auto run = sample_run();
    run.id = "501";
    run.status = labbridge::core::TaskRunStatus::Succeeded;
    run.finished_at = "2026-08-11T10:01:00Z";
    run.items_total = 2;
    run.items_success = 2;

    repository.finish(run);

    ASSERT_EQ(session.executions.size(), 1U);
    const auto& statement = session.executions.front();
    EXPECT_NE(statement.sql.find("UPDATE task_runs"), std::string::npos);
    EXPECT_EQ(statement.params,
              (labbridge::server::SqlParams{
                  "501", "succeeded", "2026-08-11T10:01:00Z",
                  "2", "2", "0", ""}));
}

TEST(PostgresTaskRunRepositoryTest, ScheduledInsertReturnsCreatedIdentity) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "502"}}};
    };
    PostgresTaskRunRepository repository{session};
    auto run = sample_run();
    run.execution_key = "scheduled-key";
    run.scheduled_for = "2026-08-11T10:00:00Z";

    const auto result = repository.create_or_find_scheduled(run);

    EXPECT_TRUE(result.created);
    EXPECT_EQ(result.task_run.id, "502");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    EXPECT_NE(session.query_one_calls.front().sql.find("ON CONFLICT"),
              std::string::npos);
    ASSERT_EQ(session.query_one_calls.front().params.size(), 11U);
    EXPECT_EQ(session.query_one_calls.front().params[9], "scheduled-key");
}

}  // namespace
