#include "labbridge/server/application/agent_control_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/node_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace {

class ExecutableConfigPostgresTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<labbridge::server::LibpqSqlSession>(
            std::getenv("LABBRIDGE_DATABASE_URL"));
        session_->execute("BEGIN", {});
        transaction_active_ = true;
        const auto transaction = session_->query_one(
            "SELECT txid_current()::text AS id",
            {});
        ASSERT_TRUE(transaction.has_value());
        node_code_ = "phase022-config-" +
            labbridge::server::storage::value_or_empty(*transaction, "id");
    }

    void TearDown() override {
        if (!transaction_active_) {
            return;
        }
        try {
            session_->execute("ROLLBACK", {});
        } catch (const std::exception& error) {
            ADD_FAILURE() << "failed to roll back PostgreSQL fixture: "
                          << error.what();
        }
        transaction_active_ = false;
    }

    labbridge::server::LibpqSqlSession& session() {
        return *session_;
    }

    const std::string& node_code() const {
        return node_code_;
    }

private:
    std::unique_ptr<labbridge::server::LibpqSqlSession> session_;
    std::string node_code_;
    bool transaction_active_{false};
};

TEST_F(ExecutableConfigPostgresTest,
       ProjectsEnabledSourceAndRulesInStableOrder) {
    labbridge::server::PostgresNodeRepository nodes{session()};
    labbridge::server::PostgresConfigRepository configs{session()};
    labbridge::server::PostgresQcRepository qc{session()};
    labbridge::server::NodeService node_service{nodes};
    labbridge::server::ConfigService config_service{nodes, configs};
    labbridge::server::AgentControlService service{
        node_service,
        config_service};

    ASSERT_TRUE(service.register_node({
        node_code(),
        "phase022 PostgreSQL node",
        "0.1.0",
    }).ok);
    const auto source = config_service.create_data_source({
        node_code(),
        labbridge::core::SourceType::LocalDirectory,
        "phase022 enabled source",
        R"({"root_path":"/data/incoming","extension":".csv"})",
        true,
    });
    const auto disabled_source = config_service.create_data_source({
        node_code(),
        labbridge::core::SourceType::LocalDirectory,
        "phase022 disabled source",
        R"({"root_path":"/data/disabled","extension":".csv"})",
        false,
    });
    ASSERT_TRUE(source.status.ok);
    ASSERT_TRUE(disabled_source.status.ok);

    const auto task = config_service.create_task({
        node_code(),
        source.id,
        "phase022 executable task",
        "local_file_import",
        "*/5 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    const auto incomplete_task = config_service.create_task({
        node_code(),
        disabled_source.id,
        "phase022 incomplete task",
        "local_file_import",
        "*/5 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    ASSERT_TRUE(task.status.ok);
    ASSERT_TRUE(incomplete_task.status.ok);

    const auto timestamp_rule = qc.create_rule({
        {},
        "timestamp",
        "basic_timestamp_format",
        "{}",
        true,
    });
    const auto required_rule = qc.create_rule({
        {},
        "required",
        "required_fields",
        "{}",
        true,
    });
    const auto disabled_rule = qc.create_rule({
        {},
        "disabled",
        "required_fields",
        "{}",
        false,
    });
    configs.bind_task_qc_rule(task.id, timestamp_rule, 20);
    configs.bind_task_qc_rule(task.id, required_rule, 10);
    configs.bind_task_qc_rule(task.id, disabled_rule, 0);
    configs.bind_task_qc_rule(task.id, required_rule, 5);

    const auto projection = service.find_config(node_code());
    ASSERT_TRUE(projection.status.ok);
    ASSERT_EQ(projection.enabled_tasks.size(), 1U);
    ASSERT_EQ(projection.data_sources.size(), 1U);
    ASSERT_EQ(projection.task_qc_rules.size(), 2U);
    EXPECT_EQ(projection.enabled_tasks.front().id, task.id);
    EXPECT_EQ(
        projection.enabled_tasks.front().qc_rule_ids,
        (std::vector<std::string>{required_rule, timestamp_rule}));
    EXPECT_EQ(projection.data_sources.front().id, source.id);
    EXPECT_EQ(projection.task_qc_rules[0].sort_order, 5);
    EXPECT_EQ(projection.task_qc_rules[1].sort_order, 20);

    const auto binding_count = session().query_one(
        "SELECT count(*)::text AS count "
        "FROM task_qc_rules WHERE task_id = $1::bigint "
        "AND qc_rule_id = $2::bigint",
        {task.id, required_rule});
    ASSERT_TRUE(binding_count.has_value());
    EXPECT_EQ(
        labbridge::server::storage::value_or_empty(
            *binding_count, "count"),
        "1");

    session().execute("SAVEPOINT invalid_binding", {});
    EXPECT_THROW(
        configs.bind_task_qc_rule(task.id, "999999999999", 30),
        std::runtime_error);
    session().execute("ROLLBACK TO SAVEPOINT invalid_binding", {});
}

}  // namespace

int main(int argc, char** argv) {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string{connection_info}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping executable "
                     "config PostgreSQL test\n";
        return 77;
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
