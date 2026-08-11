#include "labbridge/server/postgres/alert_repository.h"
#include "support/server/recording_sql_session.h"

#include <gtest/gtest.h>

namespace {

using labbridge::server::PostgresAlertRepository;
using labbridge::server::SqlRow;
using labbridge::server::test_support::RecordingSqlSession;

labbridge::server::SqlRow alert_row() {
    return {
        {"id", "1401"},
        {"node_code", "unit-node"},
        {"task_run_id", "501"},
        {"alert_type", "qc_result"},
        {"severity", "warning"},
        {"message", "near upper limit"},
        {"status", "open"},
    };
}

TEST(PostgresAlertRepositoryTest, CreateDefaultsEmptyStatusToOpen) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "1401"}}};
    };
    PostgresAlertRepository repository{session};

    const auto id = repository.create({
        {}, "unit-node", "501", "qc_result", "warning",
        "near upper limit", {},
    });

    EXPECT_EQ(id, "1401");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    const auto& statement = session.query_one_calls.front();
    EXPECT_NE(statement.sql.find("INSERT INTO alerts"), std::string::npos);
    ASSERT_EQ(statement.params.size(), 6U);
    EXPECT_EQ(statement.params[0], "unit-node");
    EXPECT_EQ(statement.params[5], "open");
}

TEST(PostgresAlertRepositoryTest, FindByIdMapsAlert) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{alert_row()};
    };
    PostgresAlertRepository repository{session};

    const auto alert = repository.find_by_id("1401");

    ASSERT_TRUE(alert.has_value());
    EXPECT_EQ(alert->node_code, "unit-node");
    EXPECT_EQ(alert->task_run_id, "501");
    EXPECT_EQ(alert->severity, "warning");
    EXPECT_EQ(alert->status, "open");
}

TEST(PostgresAlertRepositoryTest, FindByNodeMapsRows) {
    RecordingSqlSession session;
    session.on_query_all = [](const std::string&, const auto&) {
        return std::vector<SqlRow>{alert_row()};
    };
    PostgresAlertRepository repository{session};

    const auto alerts = repository.find_by_node("unit-node");

    ASSERT_EQ(alerts.size(), 1U);
    EXPECT_EQ(alerts.front().id, "1401");
    ASSERT_EQ(session.query_all_calls.size(), 1U);
    EXPECT_NE(session.query_all_calls.front().sql.find(
                  "WHERE n.node_code = $1"),
              std::string::npos);
}

TEST(PostgresAlertRepositoryTest, FindByTaskRunUsesRunFilter) {
    RecordingSqlSession session;
    session.on_query_all = [](const std::string&, const auto&) {
        return std::vector<SqlRow>{alert_row()};
    };
    PostgresAlertRepository repository{session};

    const auto alerts = repository.find_by_task_run("501");

    ASSERT_EQ(alerts.size(), 1U);
    EXPECT_EQ(alerts.front().task_run_id, "501");
    ASSERT_EQ(session.query_all_calls.size(), 1U);
    EXPECT_EQ(session.query_all_calls.front().params,
              labbridge::server::SqlParams{"501"});
}

}  // namespace
