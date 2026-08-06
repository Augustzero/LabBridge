#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/core/version.h"

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

TEST(AgentConfigTest, ParsesRequiredFieldsFromYamlContent) {
    const auto config = labbridge::agent::parse_agent_config(R"(
agent:
  node_code: phase20-node
  name: phase20 agent
  server_url: http://127.0.0.1:18080/
  request_timeout_seconds: 7
)");

    EXPECT_EQ(config.node.node_code, "phase20-node");
    EXPECT_EQ(config.node.name, "phase20 agent");
    EXPECT_EQ(config.node.agent_version, labbridge::core::kVersion);
    EXPECT_EQ(config.server_url, "http://127.0.0.1:18080/");
    EXPECT_EQ(config.request_timeout, 7s);
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

}  // namespace
