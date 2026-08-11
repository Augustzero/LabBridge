#include "labbridge/server/postgres/config_repository.h"
#include "support/server/recording_sql_session.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

using labbridge::server::PostgresConfigRepository;
using labbridge::server::SqlRow;
using labbridge::server::test_support::RecordingSqlSession;

TEST(PostgresConfigRepositoryTest, CreateDataSourceMapsSqlAndReturnsId) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "101"}}};
    };
    PostgresConfigRepository repository{session};

    const auto id = repository.create_data_source({
        {},
        "unit-node",
        labbridge::core::SourceType::LocalDirectory,
        "CSV inbox",
        R"({"root_path":"/inbox"})",
        true,
    });

    EXPECT_EQ(id, "101");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    const auto& statement = session.query_one_calls.front();
    EXPECT_NE(statement.sql.find("INSERT INTO data_sources"), std::string::npos);
    ASSERT_EQ(statement.params.size(), 5U);
    EXPECT_EQ(statement.params[0], "unit-node");
    EXPECT_EQ(statement.params[1], "local_directory");
    EXPECT_EQ(statement.params[4], "true");
}

TEST(PostgresConfigRepositoryTest, CreateDataSourceRejectsMissingReturningRow) {
    RecordingSqlSession session;
    PostgresConfigRepository repository{session};

    EXPECT_THROW(
        repository.create_data_source({
            {}, "unit-node", labbridge::core::SourceType::LocalDirectory,
            "CSV inbox", "{}", true}),
        std::runtime_error);
}

TEST(PostgresConfigRepositoryTest, FindDataSourceMapsStorageRow) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{
            {"id", "101"},
            {"node_code", "unit-node"},
            {"source_type", "local_directory"},
            {"name", "CSV inbox"},
            {"config_json", "{}"},
            {"enabled", "true"},
        }};
    };
    PostgresConfigRepository repository{session};

    const auto source = repository.find_data_source("101");

    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->id, "101");
    EXPECT_EQ(source->node_code, "unit-node");
    EXPECT_EQ(source->source_type, labbridge::core::SourceType::LocalDirectory);
    EXPECT_TRUE(source->enabled);
}

TEST(PostgresConfigRepositoryTest, CreateTaskMapsSqlAndReturnsId) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "202"}}};
    };
    PostgresConfigRepository repository{session};

    labbridge::server::TaskRecord task;
    task.node_code = "unit-node";
    task.data_source_id = "101";
    task.name = "CSV task";
    task.task_type = "local_file_import";
    task.schedule_expr = "*/5 * * * *";
    task.parser_type = "csv_observation";
    task.qc_profile = "basic";
    task.enabled = true;
    const auto id = repository.create_task(task);

    EXPECT_EQ(id, "202");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    const auto& statement = session.query_one_calls.front();
    EXPECT_NE(statement.sql.find("INSERT INTO tasks"), std::string::npos);
    ASSERT_EQ(statement.params.size(), 8U);
    EXPECT_EQ(statement.params[0], "unit-node");
    EXPECT_EQ(statement.params[1], "101");
    EXPECT_EQ(statement.params[7], "true");
}

TEST(PostgresConfigRepositoryTest, FindEnabledTasksMapsRows) {
    RecordingSqlSession session;
    session.on_query_all = [](const std::string&, const auto&) {
        return std::vector<SqlRow>{SqlRow{
            {"id", "202"},
            {"node_code", "unit-node"},
            {"data_source_id", "101"},
            {"name", "CSV task"},
            {"task_type", "local_file_import"},
            {"schedule_expr", "*/5 * * * *"},
            {"parser_type", "csv_observation"},
            {"qc_profile", "basic"},
            {"enabled", "true"},
        }};
    };
    PostgresConfigRepository repository{session};

    const auto tasks = repository.find_enabled_tasks_by_node("unit-node");

    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().id, "202");
    EXPECT_EQ(tasks.front().data_source_id, "101");
    EXPECT_EQ(tasks.front().parser_type, "csv_observation");
    ASSERT_EQ(session.query_all_calls.size(), 1U);
    EXPECT_EQ(session.query_all_calls.front().params,
              labbridge::server::SqlParams{"unit-node"});
}

TEST(PostgresConfigRepositoryTest, BindQcRuleUsesStableUpsertParameters) {
    RecordingSqlSession session;
    PostgresConfigRepository repository{session};

    repository.bind_task_qc_rule("202", "303", 10);

    ASSERT_EQ(session.executions.size(), 1U);
    EXPECT_NE(session.executions.front().sql.find("ON CONFLICT"),
              std::string::npos);
    EXPECT_EQ(session.executions.front().params,
              (labbridge::server::SqlParams{"202", "303", "10"}));
}

}  // namespace
