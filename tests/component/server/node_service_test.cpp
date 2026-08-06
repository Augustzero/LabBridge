#include "labbridge/server/application/node_service.h"
#include "support/server/in_memory_repositories.h"

#include <gtest/gtest.h>

namespace {

TEST(NodeServiceTest, RejectsMissingRegistrationFields) {
    labbridge::server::InMemoryNodeRepository repository;
    labbridge::server::NodeService service{repository};

    const auto missing_code =
        service.register_node({"", "node name", "0.1.0"});
    const auto missing_name =
        service.register_node({"node-a", "", "0.1.0"});

    EXPECT_FALSE(missing_code.ok);
    EXPECT_FALSE(missing_name.ok);
    EXPECT_FALSE(missing_code.message.empty());
    EXPECT_FALSE(missing_name.message.empty());
}

TEST(NodeServiceTest, RegistersNodeOfflineWithSuppliedIdentity) {
    labbridge::server::InMemoryNodeRepository repository;
    labbridge::server::NodeService service{repository};

    const auto status =
        service.register_node({"node-a", "Node A", "0.1.0"});

    ASSERT_TRUE(status.ok) << status.message;
    const auto node = service.find_node("node-a");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->info.node_code, "node-a");
    EXPECT_EQ(node->info.name, "Node A");
    EXPECT_EQ(node->info.agent_version, "0.1.0");
    EXPECT_EQ(node->status, labbridge::core::NodeStatus::Offline);
    EXPECT_TRUE(node->last_heartbeat_at.empty());
}

TEST(NodeServiceTest, RejectsMissingHeartbeatFields) {
    labbridge::server::InMemoryNodeRepository repository;
    labbridge::server::NodeService service{repository};

    const auto missing_code =
        service.accept_heartbeat({"", "0.1.0", "2026-07-31 10:00:00"});
    const auto missing_time =
        service.accept_heartbeat({"node-a", "0.1.0", ""});

    EXPECT_FALSE(missing_code.ok);
    EXPECT_FALSE(missing_time.ok);
    EXPECT_FALSE(missing_code.message.empty());
    EXPECT_FALSE(missing_time.message.empty());
}

TEST(NodeServiceTest, ReturnsNotFoundForUnregisteredHeartbeat) {
    labbridge::server::InMemoryNodeRepository repository;
    labbridge::server::NodeService service{repository};

    const auto status = service.accept_heartbeat(
        {"missing", "0.1.0", "2026-07-31 10:00:00"});

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.code, labbridge::core::StatusCode::NotFound);
}

TEST(NodeServiceTest, HeartbeatMarksRegisteredNodeOnlineAndUpdatesFields) {
    labbridge::server::InMemoryNodeRepository repository;
    labbridge::server::NodeService service{repository};
    const auto registration =
        service.register_node({"node-a", "Node A", "0.1.0"});
    ASSERT_TRUE(registration.ok) << registration.message;

    const auto heartbeat = service.accept_heartbeat(
        {"node-a", "0.1.1", "2026-07-31 10:00:00"});

    ASSERT_TRUE(heartbeat.ok) << heartbeat.message;
    const auto node = service.find_node("node-a");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->status, labbridge::core::NodeStatus::Online);
    EXPECT_EQ(node->info.agent_version, "0.1.1");
    EXPECT_EQ(node->last_heartbeat_at, "2026-07-31 10:00:00");
}

}  // namespace
