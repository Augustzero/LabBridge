#include "labbridge/server/postgres/node_repository.h"
#include "support/server/recording_sql_session.h"

#include <gtest/gtest.h>

namespace {

using labbridge::server::PostgresNodeRepository;
using labbridge::server::SqlRow;
using labbridge::server::test_support::RecordingSqlSession;

TEST(PostgresNodeRepositoryTest, UpsertMapsNodeFieldsToParameterizedSql) {
    RecordingSqlSession session;
    PostgresNodeRepository repository{session};

    labbridge::server::NodeRecord node;
    node.info.node_code = "unit-node";
    node.info.name = "Unit node";
    node.info.agent_version = "0.1.0";
    node.status = labbridge::core::NodeStatus::Online;
    node.last_heartbeat_at = "2026-08-11 10:00:00";
    repository.upsert(node);

    ASSERT_EQ(session.executions.size(), 1U);
    const auto& statement = session.executions.front();
    EXPECT_NE(statement.sql.find("INSERT INTO nodes"), std::string::npos);
    EXPECT_NE(statement.sql.find("ON CONFLICT (node_code)"), std::string::npos);
    ASSERT_EQ(statement.params.size(), 5U);
    EXPECT_EQ(statement.params[0], "unit-node");
    EXPECT_EQ(statement.params[2], "online");
    EXPECT_EQ(statement.params[4], "2026-08-11 10:00:00");
}

TEST(PostgresNodeRepositoryTest, FindByCodeMapsStorageRow) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{
            {"node_code", "unit-node"},
            {"name", "Unit node"},
            {"status", "online"},
            {"agent_version", "0.1.0"},
            {"last_heartbeat_at", "2026-08-11 10:00:00"},
        }};
    };
    PostgresNodeRepository repository{session};

    const auto node = repository.find_by_code("unit-node");

    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->info.node_code, "unit-node");
    EXPECT_EQ(node->info.name, "Unit node");
    EXPECT_EQ(node->status, labbridge::core::NodeStatus::Online);
    EXPECT_EQ(node->last_heartbeat_at, "2026-08-11 10:00:00");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    EXPECT_EQ(session.query_one_calls.front().params,
              labbridge::server::SqlParams{"unit-node"});
}

}  // namespace
