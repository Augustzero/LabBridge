#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/storage_mapping.h"
#include "labbridge/server/postgres/task_run_executor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <stdexcept>
#include <thread>

namespace {

class PersistentFixture {
public:
    explicit PersistentFixture(const std::string& connection_info)
        : session_(connection_info),
          node_code_(
              "phase022-start-" +
              std::to_string(
                  std::chrono::steady_clock::now()
                      .time_since_epoch()
                      .count())) {
        const auto node = session_.query_one(
            "INSERT INTO nodes (node_code, name, status, agent_version) "
            "VALUES ($1, 'Phase 022 start fixture', 'online', '0.1.0') "
            "RETURNING id::text AS id",
            {node_code_});
        if (!node.has_value()) {
            throw std::runtime_error("failed to create start fixture node");
        }
        node_id_ = labbridge::server::storage::value_or_empty(*node, "id");

        const auto source = session_.query_one(
            "INSERT INTO data_sources "
            "(node_id, source_type, name, config_json, enabled) "
            "VALUES ($1::bigint, 'local_directory', 'start fixture source', "
            "jsonb_build_object('root_path', "
            "'/home/lenovo/labbridge/tests/fixtures/agent', "
            "'extension', '.csv'), true) RETURNING id::text AS id",
            {node_id_});
        if (!source.has_value()) {
            throw std::runtime_error("failed to create start fixture source");
        }
        source_id_ =
            labbridge::server::storage::value_or_empty(*source, "id");
        first_task_id_ = create_task("first scheduled task");
        second_task_id_ = create_task("second scheduled task");
    }

    ~PersistentFixture() {
        try {
            session_.execute(
                "DELETE FROM task_runs WHERE node_id = $1::bigint",
                {node_id_});
            session_.execute(
                "DELETE FROM tasks WHERE node_id = $1::bigint",
                {node_id_});
            session_.execute(
                "DELETE FROM data_sources WHERE id = $1::bigint",
                {source_id_});
            session_.execute(
                "DELETE FROM nodes WHERE id = $1::bigint",
                {node_id_});
        } catch (const std::exception& error) {
            ADD_FAILURE() << "failed to clean task run start fixture: "
                          << error.what();
        } catch (...) {
            ADD_FAILURE() << "unknown task run start fixture cleanup failure";
        }
    }

    const std::string& node_code() const {
        return node_code_;
    }

    const std::string& node_id() const {
        return node_id_;
    }

    const std::string& first_task_id() const {
        return first_task_id_;
    }

    const std::string& second_task_id() const {
        return second_task_id_;
    }

    labbridge::server::ISqlSession& session() {
        return session_;
    }

private:
    std::string create_task(const std::string& name) {
        const auto task = session_.query_one(
            "INSERT INTO tasks "
            "(node_id, data_source_id, name, task_type, schedule_expr, "
            "parser_type, qc_profile, enabled) "
            "VALUES ($1::bigint, $2::bigint, $3, 'local_file_import', "
            "'*/5 * * * *', 'csv_observation', 'basic', true) "
            "RETURNING id::text AS id",
            {node_id_, source_id_, name});
        if (!task.has_value()) {
            throw std::runtime_error("failed to create start fixture task");
        }
        return labbridge::server::storage::value_or_empty(*task, "id");
    }

    labbridge::server::LibpqSqlSession session_;
    std::string node_code_;
    std::string node_id_;
    std::string source_id_;
    std::string first_task_id_;
    std::string second_task_id_;
};

labbridge::server::StartTaskRunRequest start_request(
    const PersistentFixture& fixture,
    const std::string& task_id,
    const std::string& key,
    const std::string& scheduled_for) {
    labbridge::server::StartTaskRunRequest request;
    request.node_code = fixture.node_code();
    request.task_id = task_id;
    request.started_at = "2026-08-08T10:00:01Z";
    request.trigger_type = "scheduled";
    request.execution_key = key;
    request.scheduled_for = scheduled_for;
    return request;
}

TEST(TaskRunStartPostgresTest, PersistsReplayConflictAndConcurrentUniqueness) {
    const std::string connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    PersistentFixture fixture{connection_info};
    labbridge::server::PostgresTaskRunExecutor executor{connection_info};

    const auto request = start_request(
        fixture,
        fixture.first_task_id(),
        "phase022-start-sequential",
        "2026-08-08T10:00:00Z");
    const auto created = executor.start(request);
    const auto replayed = executor.start(request);

    ASSERT_TRUE(created.status.ok) << created.status.message;
    ASSERT_TRUE(replayed.status.ok) << replayed.status.message;
    EXPECT_FALSE(created.replayed);
    EXPECT_TRUE(replayed.replayed);
    EXPECT_EQ(replayed.id, created.id);

    const auto task_conflict = executor.start(start_request(
        fixture,
        fixture.second_task_id(),
        request.execution_key,
        request.scheduled_for));
    const auto slot_conflict = executor.start(start_request(
        fixture,
        fixture.first_task_id(),
        request.execution_key,
        "2026-08-08T10:05:00Z"));
    EXPECT_FALSE(task_conflict.status.ok);
    EXPECT_EQ(task_conflict.status.code, labbridge::core::StatusCode::Conflict);
    EXPECT_FALSE(slot_conflict.status.ok);
    EXPECT_EQ(slot_conflict.status.code, labbridge::core::StatusCode::Conflict);

    const auto concurrent_request = start_request(
        fixture,
        fixture.first_task_id(),
        "phase022-start-concurrent",
        "2026-08-08T10:10:00Z");
    labbridge::server::TaskRunCreateResult first;
    labbridge::server::TaskRunCreateResult second;
    std::thread first_thread([&] { first = executor.start(concurrent_request); });
    std::thread second_thread([&] { second = executor.start(concurrent_request); });
    first_thread.join();
    second_thread.join();

    ASSERT_TRUE(first.status.ok) << first.status.message;
    ASSERT_TRUE(second.status.ok) << second.status.message;
    EXPECT_NE(first.replayed, second.replayed);
    EXPECT_EQ(first.id, second.id);

    const auto rows = fixture.session().query_one(
        "SELECT count(*)::text AS count, "
        "min(tr.status) AS status, min(tr.trigger_type) AS trigger_type, "
        "min(tr.execution_key) AS execution_key, "
        "min(to_char(tr.scheduled_for AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')) AS scheduled_for "
        "FROM task_runs tr "
        "WHERE tr.node_id = $1::bigint",
        {fixture.node_id()});
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(
        labbridge::server::storage::value_or_empty(*rows, "count"),
        "2");
    EXPECT_EQ(
        labbridge::server::storage::value_or_empty(*rows, "status"),
        "running");
    EXPECT_EQ(
        labbridge::server::storage::value_or_empty(*rows, "trigger_type"),

        "scheduled");
    const auto persisted = fixture.session().query_one(
        "SELECT execution_key, "
        "to_char(scheduled_for AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS scheduled_for "
        "FROM task_runs WHERE id = $1::bigint",
        {created.id});
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(
        labbridge::server::storage::value_or_empty(
            *persisted, "execution_key"),
        request.execution_key);
    EXPECT_EQ(
        labbridge::server::storage::value_or_empty(
            *persisted, "scheduled_for"),
        request.scheduled_for);
}

}  // namespace

int main(int argc, char** argv) {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string{connection_info}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping task run "
                     "start PostgreSQL test\n";
        return 77;
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
