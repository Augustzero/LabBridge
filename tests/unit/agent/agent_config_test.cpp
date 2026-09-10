#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/core/version.h"
#include "labbridge/core/filesystem.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using namespace std::chrono_literals;

// token_file 是必填项且加载时真实读文件，用临时目录提供一份合法测试密钥。
class AgentConfigTest : public testing::Test {
public:
    // 供 runtime_config 组装 agent 段的 token_file 行。
    std::string token_file_line() const {
        return "  token_file: " + token_file_.string() + "\n";
    }

protected:
    void SetUp() override {
        token_directory_ = labbridge::core::fs::temp_directory_path() /
                           ("labbridge-agent-config-" +
                            std::to_string(static_cast<long>(::getpid())));
        labbridge::core::fs::create_directories(token_directory_);
        token_file_ = token_directory_ / "agent.token";
        write_token_file(std::string(64, 'a') + "\n");
    }

    void TearDown() override {
        std::error_code ignored;
        labbridge::core::fs::remove_all(token_directory_, ignored);
    }

    void write_token_file(const std::string& content) const {
        std::ofstream output{token_file_, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(output) << "cannot write test token file";
        output << content;
    }

    labbridge::core::fs::path token_directory_;
    labbridge::core::fs::path token_file_;
};

void expect_config_error(std::string_view yaml_content,
                         std::string_view message_part) {
    try {
        static_cast<void>(labbridge::agent::parse_agent_config(yaml_content));
        FAIL() << "expected AgentConfigError";
    } catch (const labbridge::agent::AgentConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find(message_part), std::string::npos);
    }
}

std::string runtime_config(const AgentConfigTest& fixture,
                           std::string_view heartbeat_line,
                           std::string_view tasks_section) {
    return std::string{R"(agent:
  node_code: phase21-node
  name: phase21 agent
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 7
)"} + std::string{heartbeat_line} + fixture.token_file_line() +
           std::string{tasks_section} + R"(
storage:
  work_dir: ./work
)";
}

TEST_F(AgentConfigTest, ParsesRequiredFieldsFromYamlContent) {
    const auto config = labbridge::agent::parse_agent_config(R"(
agent:
  node_code: phase20-node
  name: phase20 agent
  server_url: http://127.0.0.1:18080/
  request_timeout_seconds: 7
  heartbeat_interval_seconds: 15
  token_file: )" + token_file_.string() + R"(

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
    // 结尾换行被剥离后就是一个 64 位小写十六进制密钥。
    EXPECT_EQ(config.auth_token, std::string(64, 'a'));
    EXPECT_TRUE(labbridge::core::fs::path{config.work_dir}.is_absolute());
    ASSERT_EQ(config.allowed_local_roots.size(), 1U);
    EXPECT_EQ(config.allowed_local_roots.front(), "/srv/labbridge/inbox");
}

TEST_F(AgentConfigTest, AcceptsTokenFileWithoutTrailingNewline) {
    write_token_file(std::string(64, 'b'));
    const auto config = labbridge::agent::parse_agent_config(
        "agent:\n"
        "  node_code: phase20-node\n"
        "  name: phase20 agent\n"
        "  server_url: http://127.0.0.1:18080\n"
        "  request_timeout_seconds: 7\n"
        "  heartbeat_interval_seconds: 15\n" +
        token_file_line() +
        "storage:\n"
        "  queue_db: ./queue/agent.db\n"
        "  work_dir: ./work\n"
        "  max_pending_jobs: 1000\n"
        "  processed_fingerprint_capacity_per_task: 10000\n"
        "delivery:\n"
        "  retry_initial_seconds: 2\n"
        "  retry_max_seconds: 300\n"
        "tasks:\n"
        "  poll_interval_seconds: 10\n"
        "  allowed_local_roots:\n"
        "    - /srv/labbridge/inbox\n");
    EXPECT_EQ(config.auth_token, std::string(64, 'b'));
}

TEST_F(AgentConfigTest, RejectsMissingTokenFileField) {
    expect_config_error(
        R"(
agent:
  node_code: phase20-node
  name: phase20 agent
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 7
  heartbeat_interval_seconds: 15

storage:
  work_dir: ./work
)",
        "agent.token_file");
}

TEST_F(AgentConfigTest, RejectsUnreadableTokenFile) {
    expect_config_error(
        "agent:\n"
        "  node_code: phase20-node\n"
        "  name: phase20 agent\n"
        "  server_url: http://127.0.0.1:18080\n"
        "  request_timeout_seconds: 7\n"
        "  heartbeat_interval_seconds: 15\n"
        "  token_file: " + token_directory_.string() + "/missing.token\n"
        "storage:\n"
        "  work_dir: ./work\n",
        "cannot be read");
}

TEST_F(AgentConfigTest, RejectsInvalidTokenFormat) {
    // 错误信息只描述文件和格式要求，不回显文件内容。
    write_token_file("not-a-valid-token\n");
    expect_config_error(
        "agent:\n"
        "  node_code: phase20-node\n"
        "  name: phase20 agent\n"
        "  server_url: http://127.0.0.1:18080\n"
        "  request_timeout_seconds: 7\n"
        "  heartbeat_interval_seconds: 15\n" +
        token_file_line() +
        "storage:\n"
        "  work_dir: ./work\n",
        "must contain a single 64-character lowercase hex token");
}

TEST_F(AgentConfigTest, RejectsMissingRequiredName) {
    expect_config_error(R"(
agent:
  node_code: phase20-node
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 7
)",
                        "agent.name");
}

TEST_F(AgentConfigTest, RejectsOutOfRangeRequestTimeout) {
    expect_config_error(R"(
agent:
  node_code: phase20-node
  name: phase20 agent
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 301
)",
                        "between 1 and 300");
}

TEST_F(AgentConfigTest, RejectsUnsupportedServerScheme) {
    expect_config_error(R"(
agent:
  node_code: phase20-node
  name: phase20 agent
  server_url: https://127.0.0.1:18080
  request_timeout_seconds: 7
)",
                        "invalid agent.server_url");
}

TEST_F(AgentConfigTest, RejectsMissingOrInvalidHeartbeatInterval) {
    expect_config_error(
        runtime_config(*this, "", "tasks:\n  poll_interval_seconds: 10\n"),
        "agent.heartbeat_interval_seconds");
    expect_config_error(
        runtime_config(
            *this, "  heartbeat_interval_seconds: fifteen\n",
            "tasks:\n  poll_interval_seconds: 10\n"),
        "agent.heartbeat_interval_seconds must be an integer");
}

TEST_F(AgentConfigTest, RejectsHeartbeatIntervalOutsideDedicatedBounds) {
    for (const auto* value : {"0", "-1", "86401"}) {
        SCOPED_TRACE(value);
        expect_config_error(
            runtime_config(
                *this,
                "  heartbeat_interval_seconds: " + std::string{value} + "\n",
                "tasks:\n  poll_interval_seconds: 10\n"),
            "agent.heartbeat_interval_seconds must be between 1 and 86400");
    }
}

TEST_F(AgentConfigTest, RejectsMissingOrInvalidTasksSection) {
    expect_config_error(
        runtime_config(*this, "  heartbeat_interval_seconds: 15\n", ""),
        "tasks configuration section is required");
    expect_config_error(
        runtime_config(*this, "  heartbeat_interval_seconds: 15\n", "tasks: []\n"),
        "tasks configuration section is required");
}

TEST_F(AgentConfigTest, RejectsMissingOrInvalidConfigPollInterval) {
    expect_config_error(
        runtime_config(*this, "  heartbeat_interval_seconds: 15\n", "tasks: {}\n"),
        "tasks.poll_interval_seconds");
    expect_config_error(
        runtime_config(
            *this,
            "  heartbeat_interval_seconds: 15\n",
            "tasks:\n  poll_interval_seconds: ten\n"),
        "tasks.poll_interval_seconds must be an integer");
}

TEST_F(AgentConfigTest, RejectsConfigPollIntervalOutsideDedicatedBounds) {
    for (const auto* value : {"0", "-1", "86401"}) {
        SCOPED_TRACE(value);
        expect_config_error(
            runtime_config(
                *this,
                "  heartbeat_interval_seconds: 15\n",
                "tasks:\n  poll_interval_seconds: " + std::string{value} + "\n"),
            "tasks.poll_interval_seconds must be between 1 and 86400");
    }
}
TEST_F(AgentConfigTest, RejectsMissingOrRelativeAllowedLocalRoots) {
    const auto base = std::string{R"(
agent:
  node_code: phase22-node
  name: phase22 agent
  server_url: http://127.0.0.1:18080
  request_timeout_seconds: 7
  heartbeat_interval_seconds: 15
)"} + token_file_line() + R"(storage:
  work_dir: ./work
tasks:
  poll_interval_seconds: 10
)";
    expect_config_error(
        base,
        "tasks.allowed_local_roots must be a non-empty sequence");
    expect_config_error(
        base + "  allowed_local_roots:\n    - relative/inbox\n",
        "entries must be absolute paths");
}

}  // namespace
