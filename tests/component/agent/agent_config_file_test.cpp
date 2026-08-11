#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/core/filesystem.h"
#include "labbridge/core/version.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>
namespace {

using namespace std::chrono_literals;

TEST(AgentConfigFileTest, LoadsConfigurationFromFile) {
    const auto config = labbridge::agent::load_agent_config(
        "tests/fixtures/agent/phase20_agent_valid.yaml");

    EXPECT_EQ(
        config.work_dir,
        labbridge::core::fs::weakly_canonical(
            "tests/fixtures/agent/work").string());
    EXPECT_EQ(config.node.node_code, "phase20-node");
    EXPECT_EQ(config.node.name, "phase20 agent");
    EXPECT_EQ(config.node.agent_version, labbridge::core::kVersion);
    EXPECT_EQ(config.server_url, "http://127.0.0.1:18080/");
    EXPECT_EQ(config.request_timeout, 7s);
    EXPECT_EQ(config.heartbeat_interval, 15s);
    EXPECT_EQ(config.config_poll_interval, 10s);
    ASSERT_EQ(config.allowed_local_roots.size(), 1U);
    EXPECT_EQ(config.allowed_local_roots.front(), "/srv/labbridge/inbox");
    std::cout << "work_dir=" << config.work_dir
              << " allowed_local_root=" << config.allowed_local_roots.front()
              << std::endl;
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
