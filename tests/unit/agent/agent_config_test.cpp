#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/core/version.h"
#include "labbridge/core/filesystem.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

void expect_config_error(std::string_view yaml_content,
                         std::string_view message_part) {
    try {
        static_cast<void>(labbridge::agent::parse_agent_config(yaml_content));
        FAIL() << "expected AgentConfigError";
    } catch (const labbridge::agent::AgentConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find(message_part), std::string::npos);
    }
}

std::string runtime_config(std::string_view heartbeat_line,
                           std::string_view tasks_section) {
    return std::string{R"(agent:
  node_code: phase21-node
  name: phase21 agent
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 7
)"} + std::string{heartbeat_line} + std::string{tasks_section} + R"(
storage:
  work_dir: ./work
)";
}

TEST(AgentConfigTest, ParsesRequiredFieldsFromYamlContent) {
    const auto config = labbridge::agent::parse_agent_config(R"(
agent:
  node_code: phase20-node
  name: phase20 agent
  server_url: http://127.0.0.1:18080/
  request_timeout_seconds: 7
  heartbeat_interval_seconds: 15

storage:
  queue_db: ./data/agent_queue.db
  work_dir: ./data/work
  max_pending_jobs: 1000
  processed_fingerprint_capacity_per_task: 10000

delivery:
  retry_initial_seconds: 2
  retry_max_seconds: 300

tasks:
  poll_interval_seconds: 10
  allowed_local_roots:
    - /srv/labbridge/inbox
)");

    EXPECT_EQ(config.node.node_code, "phase20-node");
    EXPECT_EQ(config.node.name, "phase20 agent");
    EXPECT_EQ(config.node.agent_version, labbridge::core::kVersion);
    EXPECT_EQ(config.server_url, "http://127.0.0.1:18080/");
    EXPECT_EQ(config.request_timeout, 7s);
    EXPECT_EQ(config.heartbeat_interval, 15s);
    EXPECT_EQ(config.config_poll_interval, 10s);
    EXPECT_TRUE(labbridge::core::fs::path{config.work_dir}.is_absolute());
    ASSERT_EQ(config.allowed_local_roots.size(), 1U);
    EXPECT_EQ(config.allowed_local_roots.front(), "/srv/labbridge/inbox");
}

TEST(AgentConfigTest, RejectsMissingRequiredName) {
    expect_config_error(R"(
agent:
  node_code: phase20-node
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 7
)",
                        "agent.name");
}

TEST(AgentConfigTest, RejectsOutOfRangeRequestTimeout) {
    expect_config_error(R"(
agent:
  node_code: phase20-node
  name: phase20 agent
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 301
)",
                        "between 1 and 300");
}

TEST(AgentConfigTest, RejectsUnsupportedServerScheme) {
    expect_config_error(R"(
agent:
  node_code: phase20-node
  name: phase20 agent
  server_url: https://127.0.0.1:18080
  request_timeout_seconds: 7
)",
                        "invalid agent.server_url");
}

TEST(AgentConfigTest, RejectsMissingOrInvalidHeartbeatInterval) {
    expect_config_error(
        runtime_config("", "tasks:\n  poll_interval_seconds: 10\n"),
        "agent.heartbeat_interval_seconds");
    expect_config_error(
        runtime_config(
            "  heartbeat_interval_seconds: fifteen\n",
            "tasks:\n  poll_interval_seconds: 10\n"),
        "agent.heartbeat_interval_seconds must be an integer");
}

TEST(AgentConfigTest, RejectsHeartbeatIntervalOutsideDedicatedBounds) {
    for (const auto* value : {"0", "-1", "86401"}) {
        SCOPED_TRACE(value);
        expect_config_error(
            runtime_config(
                "  heartbeat_interval_seconds: " + std::string{value} + "\n",
                "tasks:\n  poll_interval_seconds: 10\n"),
            "agent.heartbeat_interval_seconds must be between 1 and 86400");
    }
}

TEST(AgentConfigTest, RejectsMissingOrInvalidTasksSection) {
    expect_config_error(
        runtime_config("  heartbeat_interval_seconds: 15\n", ""),
        "tasks configuration section is required");
    expect_config_error(
        runtime_config("  heartbeat_interval_seconds: 15\n", "tasks: []\n"),
        "tasks configuration section is required");
}

TEST(AgentConfigTest, RejectsMissingOrInvalidConfigPollInterval) {
    expect_config_error(
        runtime_config("  heartbeat_interval_seconds: 15\n", "tasks: {}\n"),
        "tasks.poll_interval_seconds");
    expect_config_error(
        runtime_config(
            "  heartbeat_interval_seconds: 15\n",
            "tasks:\n  poll_interval_seconds: ten\n"),
        "tasks.poll_interval_seconds must be an integer");
}

TEST(AgentConfigTest, RejectsConfigPollIntervalOutsideDedicatedBounds) {
    for (const auto* value : {"0", "-1", "86401"}) {
        SCOPED_TRACE(value);
        expect_config_error(
            runtime_config(
                "  heartbeat_interval_seconds: 15\n",
                "tasks:\n  poll_interval_seconds: " + std::string{value} + "\n"),
            "tasks.poll_interval_seconds must be between 1 and 86400");
    }
}
TEST(AgentConfigTest, RejectsMissingOrRelativeAllowedLocalRoots) {
    const auto base = R"(
agent:
  node_code: phase22-node
  name: phase22 agent
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 7
  heartbeat_interval_seconds: 15
storage:
  work_dir: ./work
tasks:
  poll_interval_seconds: 10
)";
    expect_config_error(
        base,
        "tasks.allowed_local_roots must be a non-empty sequence");
    expect_config_error(
        std::string{base} +
            "  allowed_local_roots:\n    - relative/inbox\n",
        "entries must be absolute paths");
}

}  // namespace
