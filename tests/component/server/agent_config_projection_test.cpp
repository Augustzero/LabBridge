#include "labbridge/server/application/agent_control_service.h"
#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/http/agent_control_http_controller.h"
#include "support/server/in_memory_repositories.h"

#include <gtest/gtest.h>

#include <drogon/HttpResponse.h>

#include <string>

namespace {

drogon::HttpResponsePtr invoke_config(
    const labbridge::server::AgentControlHttpController& controller,
    const std::string& node_code) {
    drogon::HttpResponsePtr response;
    controller.get_config(
        node_code,
        [&response](const drogon::HttpResponsePtr& current) {
            response = current;
        });
    EXPECT_NE(response, nullptr);
    return response;
}

TEST(AgentConfigProjectionTest, ReturnsOnlyCompleteEnabledProjection) {
    labbridge::server::InMemoryNodeRepository nodes;
    labbridge::server::InMemoryConfigRepository configs;
    labbridge::server::NodeService node_service{nodes};
    labbridge::server::ConfigService config_service{nodes, configs};
    labbridge::server::AgentControlService service{
        node_service,
        config_service};

    ASSERT_TRUE(service.register_node({"node-22", "Node 22", "0.1.0"}).ok);
    const auto enabled_source = config_service.create_data_source({
        "node-22",
        labbridge::core::SourceType::LocalDirectory,
        "enabled source",
        R"({"root_path":"/data/incoming","extension":".csv"})",
        true,
    });
    const auto disabled_source = config_service.create_data_source({
        "node-22",
        labbridge::core::SourceType::LocalDirectory,
        "disabled source",
        R"({"root_path":"/data/disabled","extension":".csv"})",
        false,
    });
    ASSERT_TRUE(enabled_source.status.ok);
    ASSERT_TRUE(disabled_source.status.ok);

    const auto task = config_service.create_task({
        "node-22",
        enabled_source.id,
        "executable task",
        "local_file_import",
        "*/5 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    const auto incomplete_task = config_service.create_task({
        "node-22",
        disabled_source.id,
        "incomplete task",
        "local_file_import",
        "*/5 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    ASSERT_TRUE(task.status.ok);
    ASSERT_TRUE(incomplete_task.status.ok);
    configs.add_task_qc_rule_projection({
        task.id,
        "rule-22",
        "required_fields",
        "required fields",
        "{}",
        10,
    });

    const auto projection = service.find_config("node-22");
    ASSERT_TRUE(projection.status.ok);
    ASSERT_EQ(projection.enabled_tasks.size(), 1U);
    ASSERT_EQ(projection.data_sources.size(), 1U);
    ASSERT_EQ(projection.task_qc_rules.size(), 1U);
    EXPECT_EQ(projection.enabled_tasks.front().id, task.id);
    EXPECT_EQ(projection.enabled_tasks.front().qc_rule_ids,
              std::vector<std::string>{"rule-22"});
    EXPECT_EQ(projection.data_sources.front().id, enabled_source.id);

    labbridge::server::AgentControlHttpController controller{
        [&service](const labbridge::core::NodeInfo& node) {
            return service.register_node(node);
        },
        [&service](const labbridge::core::NodeHeartbeat& heartbeat) {
            return service.accept_heartbeat(heartbeat);
        },
        [&service](const std::string& node_code) {
            return service.find_config(node_code);
        }};
    const auto response = invoke_config(controller, "node-22");
    ASSERT_NE(response, nullptr);
    ASSERT_EQ(response->statusCode(), drogon::k200OK);
    const auto& body = response->getJsonObject();
    ASSERT_NE(body, nullptr);
    const auto& data = (*body)["data"];
    ASSERT_EQ(data["tasks"].size(), 1U);
    ASSERT_EQ(data["data_sources"].size(), 1U);
    ASSERT_EQ(data["qc_rules"].size(), 1U);
    EXPECT_TRUE(data["data_sources"][0]["config"].isObject());
    EXPECT_EQ(
        data["data_sources"][0]["config"]["extension"].asString(),
        ".csv");
    EXPECT_TRUE(data["qc_rules"][0]["config"].isObject());
    EXPECT_EQ(data["tasks"][0]["qc_rule_ids"][0].asString(), "rule-22");
}

TEST(AgentConfigProjectionTest, SanitizesInvalidStoredJsonObject) {
    labbridge::server::AgentControlHttpController controller{
        [](const labbridge::core::NodeInfo&) {
            return labbridge::core::Status::success();
        },
        [](const labbridge::core::NodeHeartbeat&) {
            return labbridge::core::Status::success();
        },
        [](const std::string&) {
            labbridge::server::AgentConfigResult result;
            result.status = labbridge::core::Status::success();
            result.node = labbridge::server::NodeRecord{
                {"node-22", "Node 22", "0.1.0"},
                labbridge::core::NodeStatus::Online,
                "2026-08-08T00:00:00Z",
            };
            result.data_sources.push_back({
                "source-22",
                "node-22",
                labbridge::core::SourceType::LocalDirectory,
                "invalid source",
                "[]",
                true,
            });
            return result;
        }};

    const auto response = invoke_config(controller, "node-22");
    ASSERT_NE(response, nullptr);
    ASSERT_EQ(response->statusCode(), drogon::k500InternalServerError);
    const auto& body = response->getJsonObject();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ((*body)["error"]["code"].asString(), "internal_error");
    EXPECT_EQ((*body)["error"]["message"].asString(),
              "internal server error");
}

}  // namespace
