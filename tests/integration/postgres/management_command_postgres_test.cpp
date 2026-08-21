#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/management_command_service.h"
#include "labbridge/server/postgres/agent_control_executor.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/management_command_executor.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/sql_transaction.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using labbridge::server::ISqlSession;
using labbridge::server::ManagementTaskCreateRequest;
using labbridge::server::SqlParams;
using labbridge::server::SqlRow;

std::string value(const std::optional<SqlRow>& row,
                  const std::string& column) {
    if (!row.has_value()) {
        return {};
    }
    return labbridge::server::storage::value_or_empty(*row, column);
}

class FailingBindingSqlSession final : public ISqlSession {
public:
    explicit FailingBindingSqlSession(ISqlSession& delegate)
        : delegate_(delegate) {}

    void execute(const std::string& sql, const SqlParams& params) override {
        if (sql.find("INSERT INTO task_qc_rules") != std::string::npos &&
            ++binding_writes_ == 2) {
            throw std::runtime_error("injected second binding failure");
        }
        delegate_.execute(sql, params);
    }

    std::optional<SqlRow> query_one(
        const std::string& sql,
        const SqlParams& params) override {
        return delegate_.query_one(sql, params);
    }

    std::vector<SqlRow> query_all(
        const std::string& sql,
        const SqlParams& params) override {
        return delegate_.query_all(sql, params);
    }

private:
    ISqlSession& delegate_;
    int binding_writes_{0};
};

class ManagementCommandPostgresTest : public testing::Test {
protected:
    void SetUp() override {
        connection_info_ = std::getenv("LABBRIDGE_DATABASE_URL");
        session_ = std::make_unique<labbridge::server::LibpqSqlSession>(
            connection_info_);
        const auto identity = session_->query_one(
            "SELECT txid_current()::text AS id", {});
        ASSERT_TRUE(identity.has_value());
        suffix_ = value(identity, "id");
        node_code_ = "phase02503-command-" + suffix_;
        prefix_ = "phase02503-" + suffix_;
        session_->execute(
            "INSERT INTO nodes (node_code,name,status,agent_version) "
            "VALUES ($1,$2,'online','0.1.0')",
            {node_code_, prefix_ + " node"});
        executor_ =
            std::make_unique<labbridge::server::PostgresManagementCommandExecutor>(
                connection_info_);
    }

    void TearDown() override {
        if (!session_) {
            return;
        }
        try {
            // fixture 只按本测试唯一 node/name 前缀逐表清理。
            session_->execute(
                "DELETE FROM task_qc_rules WHERE task_id IN ("
                "SELECT t.id FROM tasks t JOIN nodes n ON n.id=t.node_id "
                "WHERE n.node_code=$1)",
                {node_code_});
            session_->execute(
                "DELETE FROM tasks WHERE node_id IN ("
                "SELECT id FROM nodes WHERE node_code=$1)",
                {node_code_});
            session_->execute(
                "DELETE FROM data_sources WHERE node_id IN ("
                "SELECT id FROM nodes WHERE node_code=$1)",
                {node_code_});
            session_->execute(
                "DELETE FROM qc_rules WHERE name LIKE $1",
                {prefix_ + "%"});
            session_->execute(
                "DELETE FROM nodes WHERE node_code=$1", {node_code_});
        } catch (const std::exception& error) {
            ADD_FAILURE() << "failed to clean PostgreSQL fixture: "
                          << error.what();
        }
    }

    std::string create_source(bool enabled = true) {
        const auto result = executor_->create_data_source({
            node_code_,
            labbridge::core::SourceType::LocalDirectory,
            prefix_ + " source",
            R"({"root_path":"/srv/labbridge/inbox","extension":".csv"})",
            enabled,
        });
        EXPECT_TRUE(result.status.ok) << result.status.message;
        return result.id;
    }

    std::string create_rule(const std::string& type,
                            bool enabled = true) {
        const auto result = executor_->create_qc_rule({
            prefix_ + " " + type,
            type,
            "{}",
            enabled,
        });
        EXPECT_TRUE(result.status.ok) << result.status.message;
        return result.id;
    }

    ManagementTaskCreateRequest task_request(
        const std::string& source_id,
        std::vector<std::string> rule_ids,
        const std::string& name_suffix = "task") const {
        return {
            node_code_,
            source_id,
            prefix_ + " " + name_suffix,
            "local_file_import",
            "*/5 * * * *",
            "csv_observation",
            "default",
            std::move(rule_ids),
            true,
        };
    }

    std::string connection_info_;
    std::string suffix_;
    std::string node_code_;
    std::string prefix_;
    std::unique_ptr<labbridge::server::LibpqSqlSession> session_;
    std::unique_ptr<labbridge::server::PostgresManagementCommandExecutor>
        executor_;
};

TEST_F(ManagementCommandPostgresTest,
       ExecutorCommitsExecutableConfigurationAndTaskStateChanges) {
    const auto source_id = create_source();
    const auto timestamp_id = create_rule("basic_timestamp_format");
    const auto required_id = create_rule("required_fields");
    const auto task = executor_->create_task(
        task_request(source_id, {timestamp_id, required_id}));
    ASSERT_TRUE(task.status.ok) << task.status.message;

    labbridge::server::PostgresAgentControlExecutor agent{connection_info_};
    const auto committed = agent.find_config(node_code_);
    ASSERT_TRUE(committed.status.ok) << committed.status.message;
    ASSERT_EQ(committed.enabled_tasks.size(), 1U);
    EXPECT_EQ(committed.enabled_tasks.front().qc_rule_ids,
              (std::vector<std::string>{timestamp_id, required_id}));

    ASSERT_TRUE(executor_->set_task_enabled(task.id, false).status.ok);
    const auto disabled = agent.find_config(node_code_);
    ASSERT_TRUE(disabled.status.ok);
    EXPECT_TRUE(disabled.enabled_tasks.empty());

    ASSERT_TRUE(executor_->set_task_enabled(task.id, true).status.ok);
    const auto reenabled = agent.find_config(node_code_);
    ASSERT_TRUE(reenabled.status.ok);
    ASSERT_EQ(reenabled.enabled_tasks.size(), 1U);

    std::cout << "management_command commit task=" << task.id
              << " bindings=" << timestamp_id << ',' << required_id
              << " committed_projection=" << committed.enabled_tasks.size()
              << " disabled_projection=" << disabled.enabled_tasks.size()
              << " reenabled_projection=" << reenabled.enabled_tasks.size()
              << '\n';
}

TEST_F(ManagementCommandPostgresTest,
       RejectedDependencyLeavesNoTaskOrBinding) {
    const auto source_id = create_source();
    const auto disabled_rule = create_rule("required_fields", false);
    const auto rejected = executor_->create_task(
        task_request(source_id, {disabled_rule}, "rejected-task"));

    EXPECT_FALSE(rejected.status.ok);
    EXPECT_EQ(rejected.status.code, labbridge::core::StatusCode::Conflict);
    const auto counts = session_->query_one(
        "SELECT count(*)::text AS tasks, "
        "(SELECT count(*)::text FROM task_qc_rules tqr "
        " JOIN tasks t ON t.id=tqr.task_id WHERE t.name=$1) AS bindings "
        "FROM tasks WHERE name=$1",
        {prefix_ + " rejected-task"});
    ASSERT_TRUE(counts.has_value());
    EXPECT_EQ(value(counts, "tasks"), "0");
    EXPECT_EQ(value(counts, "bindings"), "0");

    std::cout << "management_command rejected status=conflict tasks="
              << value(counts, "tasks")
              << " bindings=" << value(counts, "bindings") << '\n';
}

TEST_F(ManagementCommandPostgresTest,
       TransactionHidesPartialConfigurationAndRollsBackBindingFailure) {
    const auto source_id = create_source();
    const auto timestamp_id = create_rule("basic_timestamp_format");
    const auto required_id = create_rule("required_fields");
    const auto request = task_request(
        source_id, {timestamp_id, required_id}, "visibility-task");

    labbridge::server::LibpqSqlSession writer{connection_info_};
    labbridge::server::PostgresNodeRepository nodes{writer};
    labbridge::server::PostgresConfigRepository configs{writer};
    labbridge::server::PostgresQcRepository qc{writer};
    labbridge::server::ManagementCommandService service{nodes, configs, qc};
    labbridge::server::SqlTransaction transaction{writer};
    const auto created = service.create_task(request);
    ASSERT_TRUE(created.status.ok) << created.status.message;

    labbridge::server::PostgresAgentControlExecutor agent{connection_info_};
    const auto before_commit = agent.find_config(node_code_);
    ASSERT_TRUE(before_commit.status.ok);
    EXPECT_TRUE(before_commit.enabled_tasks.empty());
    transaction.commit();

    const auto after_commit = agent.find_config(node_code_);
    ASSERT_TRUE(after_commit.status.ok);
    ASSERT_EQ(after_commit.enabled_tasks.size(), 1U);
    EXPECT_EQ(after_commit.task_qc_rules.size(), 2U);

    const auto failing_name = prefix_ + " rollback-task";
    {
        labbridge::server::LibpqSqlSession raw_writer{connection_info_};
        FailingBindingSqlSession failing_session{raw_writer};
        labbridge::server::SqlTransaction rollback{failing_session};
        labbridge::server::PostgresNodeRepository failing_nodes{
            failing_session};
        labbridge::server::PostgresConfigRepository failing_configs{
            failing_session};
        labbridge::server::PostgresQcRepository failing_qc{failing_session};
        labbridge::server::ManagementCommandService failing_service{
            failing_nodes, failing_configs, failing_qc};
        EXPECT_THROW(
            failing_service.create_task(task_request(
                source_id,
                {timestamp_id, required_id},
                "rollback-task")),
            std::runtime_error);
    }
    const auto rolled_back = session_->query_one(
        "SELECT count(*)::text AS count FROM tasks WHERE name=$1",
        {failing_name});
    EXPECT_EQ(value(rolled_back, "count"), "0");

    std::cout << "management_command visibility before_commit="
              << before_commit.enabled_tasks.size()
              << " after_commit_tasks=" << after_commit.enabled_tasks.size()
              << " after_commit_bindings="
              << after_commit.task_qc_rules.size()
              << " rollback_tasks=" << value(rolled_back, "count") << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string{connection_info}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping management "
                     "command PostgreSQL test\n";
        return 77;
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
