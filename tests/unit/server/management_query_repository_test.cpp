#include "labbridge/server/postgres/management_query_repository.h"
#include "support/server/recording_sql_session.h"

#include <gtest/gtest.h>

namespace {

using labbridge::server::ManagementPageRequest;
using labbridge::server::PostgresManagementQueryRepository;
using labbridge::server::SqlRow;
using labbridge::server::test_support::RecordingSqlSession;

TEST(PostgresManagementQueryRepositoryTest,
     NodeFilterUsesDerivedStatusCursorAndBoundLimit) {
    RecordingSqlSession session;
    session.on_query_all = [](const std::string&, const auto&) {
        return std::vector<SqlRow>{SqlRow{
            {"id", "12"},
            {"node_code", "node-12"},
            {"name", "Node 12"},
            {"status", "online"},
            {"agent_version", "0.1.0"},
            {"last_heartbeat_at", "2026-08-19T00:00:00Z"},
            {"created_at", "2026-08-01T00:00:00Z"},
            {"updated_at", "2026-08-19T00:00:00Z"},
        }};
    };
    PostgresManagementQueryRepository repository{session};

    const auto nodes = repository.list_nodes(
        {
            labbridge::core::NodeStatus::Online,
            "2026-08-19T00:01:00Z",
            120,
        },
        {3, std::string{"20"}});

    ASSERT_EQ(nodes.size(), 1U);
    EXPECT_EQ(nodes.front().id, "12");
    ASSERT_EQ(session.query_all_calls.size(), 1U);
    const auto& statement = session.query_all_calls.front();
    EXPECT_NE(statement.sql.find("last_heartbeat_at >="),
              std::string::npos);
    EXPECT_NE(statement.sql.find("n.id < $3::bigint"),
              std::string::npos);
    EXPECT_NE(statement.sql.find("ORDER BY id DESC LIMIT $4::integer"),
              std::string::npos);
    EXPECT_EQ(
        statement.params,
        (labbridge::server::SqlParams{
            "2026-08-19T00:01:00Z", "120", "20", "3"}));
}

TEST(PostgresManagementQueryRepositoryTest,
     TaskPageAggregatesQcRulesWithoutPerTaskQuery) {
    RecordingSqlSession session;
    session.on_query_all = [](const std::string&, const auto&) {
        return std::vector<SqlRow>{SqlRow{
            {"id", "31"},
            {"node_code", "node-a"},
            {"data_source_id", "21"},
            {"name", "CSV"},
            {"task_type", "local_file_import"},
            {"schedule_expr", "* * * * *"},
            {"parser_type", "csv_observation"},
            {"qc_profile", "default"},
            {"enabled", "false"},
            {"qc_rule_ids", "7,9"},
            {"created_at", "2026-08-19T00:00:00Z"},
            {"updated_at", "2026-08-19T00:00:00Z"},
        }};
    };
    PostgresManagementQueryRepository repository{session};

    const auto tasks = repository.list_tasks_by_node(
        "node-a", {false}, {21, std::nullopt});

    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_FALSE(tasks.front().enabled);
    EXPECT_EQ(tasks.front().qc_rule_ids,
              (std::vector<std::string>{"7", "9"}));
    ASSERT_EQ(session.query_all_calls.size(), 1U);
    EXPECT_NE(session.query_all_calls.front().sql.find("string_agg"),
              std::string::npos);
    EXPECT_NE(session.query_all_calls.front().sql.find(
                  "ORDER BY tqr.sort_order"),
              std::string::npos);
}

TEST(PostgresManagementQueryRepositoryTest,
     QcResultsJoinRunAndKeepAllFiltersParameterized) {
    RecordingSqlSession session;
    session.on_query_all = [](const std::string&, const auto&) {
        return std::vector<SqlRow>{SqlRow{
            {"id", "91"},
            {"parsed_record_id", "81"},
            {"qc_rule_id", "71"},
            {"task_run_id", "61"},
            {"level", "record"},
            {"result", "failed"},
            {"message", "missing station"},
            {"created_at", "2026-08-19T00:00:00Z"},
        }};
    };
    PostgresManagementQueryRepository repository{session};

    const auto results = repository.list_qc_results_by_run(
        {"61", std::string{"failed"}},
        ManagementPageRequest{2, std::string{"100"}});

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().task_run_id, "61");
    EXPECT_EQ(results.front().parsed_record_id, "81");
    const auto& statement = session.query_all_calls.front();
    EXPECT_NE(statement.sql.find(
                  "JOIN parsed_records pr ON pr.id = qr.parsed_record_id"),
              std::string::npos);
    EXPECT_EQ(
        statement.params,
        (labbridge::server::SqlParams{"61", "failed", "100", "2"}));
}

}  // namespace
