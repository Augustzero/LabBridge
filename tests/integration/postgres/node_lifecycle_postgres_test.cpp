#include "labbridge/core/version.h"
#include "labbridge/server/libpq_sql_session.h"
#include "labbridge/server/node_service.h"
#include "labbridge/server/postgres_node_repository.h"
#include "labbridge/server/storage_mapping.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace {

class NodeLifecyclePostgresTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<labbridge::server::LibpqSqlSession>(
            std::getenv("LABBRIDGE_DATABASE_URL"));
        session_->execute("BEGIN", {});
        transaction_active_ = true;

        const auto transaction = session_->query_one(
            "SELECT txid_current()::text AS transaction_id",
            {});
        ASSERT_TRUE(transaction.has_value());
        const auto transaction_id = labbridge::server::storage::value_or_empty(
            *transaction,
            "transaction_id");
        ASSERT_FALSE(transaction_id.empty());
        node_code_ = "q0b-node-" + transaction_id;
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

TEST_F(NodeLifecyclePostgresTest, PersistsRegistrationAsOffline) {
    labbridge::server::PostgresNodeRepository repository{session()};
    labbridge::server::NodeService service{repository};

    const auto status = service.register_node({
        node_code(),
        "postgres fixture node",
        labbridge::core::kVersion,
    });
    ASSERT_TRUE(status.ok) << status.message;

    const auto stored = service.find_node(node_code());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->info.node_code, node_code());
    EXPECT_EQ(stored->info.name, "postgres fixture node");
    EXPECT_EQ(stored->info.agent_version, labbridge::core::kVersion);
    EXPECT_EQ(stored->status, labbridge::core::NodeStatus::Offline);
    EXPECT_TRUE(stored->last_heartbeat_at.empty());
}

TEST_F(NodeLifecyclePostgresTest, PersistsHeartbeatAsOnline) {
    labbridge::server::PostgresNodeRepository repository{session()};
    labbridge::server::NodeService service{repository};
    ASSERT_TRUE(service.register_node({
        node_code(),
        "postgres fixture node",
        "0.0.1",
    }).ok);

    const auto status = service.accept_heartbeat({
        node_code(),
        labbridge::core::kVersion,
        "2026-05-18 10:30:00+08",
    });
    ASSERT_TRUE(status.ok) << status.message;

    const auto stored = service.find_node(node_code());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->info.agent_version, labbridge::core::kVersion);
    EXPECT_EQ(stored->status, labbridge::core::NodeStatus::Online);
    EXPECT_FALSE(stored->last_heartbeat_at.empty());
}

}  // namespace

int main(int argc, char** argv) {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string{connection_info}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping PostgreSQL "
                     "node lifecycle test\n";
        return 77;
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
