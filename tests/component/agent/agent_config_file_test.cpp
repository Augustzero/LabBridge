#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/core/version.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

using namespace std::chrono_literals;

TEST(AgentConfigFileTest, LoadsConfigurationFromFile) {
    const auto config = labbridge::agent::load_agent_config(
        "tests/fixtures/agent/phase20_agent_valid.yaml");

    EXPECT_EQ(config.node.node_code, "phase20-node");
    EXPECT_EQ(config.node.name, "phase20 agent");
    EXPECT_EQ(config.node.agent_version, labbridge::core::kVersion);
    EXPECT_EQ(config.server_url, "http://127.0.0.1:18080/");
    EXPECT_EQ(config.request_timeout, 7s);
}

TEST(AgentConfigFileTest, ReportsMissingFilePath) {
    try {
        static_cast<void>(labbridge::agent::load_agent_config(
            "tests/fixtures/agent/agent_config_file_that_does_not_exist.yaml"));
        FAIL() << "expected AgentConfigError";
    } catch (const labbridge::agent::AgentConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("failed to load agent configuration"),
                  std::string::npos);
    }
}

}  // namespace
