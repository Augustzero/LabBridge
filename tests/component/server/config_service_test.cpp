#include "labbridge/server/application/config_service.h"
#include "labbridge/server/application/node_service.h"
#include "support/server/in_memory_repositories.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

class ConfigServiceTest : public testing::Test {
protected:
    void SetUp() override {
        const auto status = node_service_.register_node(
            {"node-a", "Node A", "0.1.0"});
        ASSERT_TRUE(status.ok) << status.message;
    }

    labbridge::server::CreateDataSourceRequest data_source_request(
        const std::string& node_code = "node-a") const {
        return {
            node_code,
            labbridge::core::SourceType::LocalDirectory,
            "observations",
            R"({"path":"/data"})",
            true,
        };
    }

    labbridge::server::CreateTaskRequest task_request(
        const std::string& data_source_id,
        const std::string& node_code = "node-a") const {
        return {
            node_code,
            data_source_id,
            "collect observations",
            "collect_parse_qc",
            "*/5 * * * *",
            "csv_observation",
            "basic",
            true,
        };
    }

    labbridge::server::InMemoryNodeRepository nodes_;
    labbridge::server::InMemoryConfigRepository configs_;
    labbridge::server::NodeService node_service_{nodes_};
    labbridge::server::ConfigService service_{nodes_, configs_};
};

TEST_F(ConfigServiceTest, RejectsMissingDataSourceFields) {
    std::vector<std::pair<std::string,
                          labbridge::server::CreateDataSourceRequest>> cases;
    auto missing_node = data_source_request();
    missing_node.node_code.clear();
    cases.emplace_back("node_code", std::move(missing_node));
    auto missing_name = data_source_request();
    missing_name.name.clear();
    cases.emplace_back("name", std::move(missing_name));
    auto missing_config = data_source_request();
    missing_config.config_json.clear();
    cases.emplace_back("config_json", std::move(missing_config));

    for (const auto& [name, request] : cases) {
        SCOPED_TRACE(name);
        const auto result = service_.create_data_source(request);
        EXPECT_FALSE(result.status.ok);
        EXPECT_TRUE(result.id.empty());
    }
}

TEST_F(ConfigServiceTest, RejectsDataSourceForUnknownNode) {
    const auto result =
        service_.create_data_source(data_source_request("missing"));

    EXPECT_FALSE(result.status.ok);
    EXPECT_TRUE(result.id.empty());
}

TEST_F(ConfigServiceTest, CreatesDataSourceWithSuppliedFields) {
    const auto request = data_source_request();

    const auto result = service_.create_data_source(request);

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_FALSE(result.id.empty());
    const auto stored = configs_.find_data_source(result.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->node_code, request.node_code);
    EXPECT_EQ(stored->source_type, request.source_type);
    EXPECT_EQ(stored->name, request.name);
    EXPECT_EQ(stored->config_json, request.config_json);
    EXPECT_EQ(stored->enabled, request.enabled);
}

TEST_F(ConfigServiceTest, RejectsMissingTaskFields) {
    const auto source = service_.create_data_source(data_source_request());
    ASSERT_TRUE(source.status.ok) << source.status.message;
    std::vector<std::pair<std::string,
                          labbridge::server::CreateTaskRequest>> cases;
    auto missing_node = task_request(source.id);
    missing_node.node_code.clear();
    cases.emplace_back("node_code", std::move(missing_node));
    auto missing_source = task_request(source.id);
    missing_source.data_source_id.clear();
    cases.emplace_back("data_source_id", std::move(missing_source));
    auto missing_name = task_request(source.id);
    missing_name.name.clear();
    cases.emplace_back("name", std::move(missing_name));
    auto missing_type = task_request(source.id);
    missing_type.task_type.clear();
    cases.emplace_back("task_type", std::move(missing_type));
    auto missing_schedule = task_request(source.id);
    missing_schedule.schedule_expr.clear();
    cases.emplace_back("schedule_expr", std::move(missing_schedule));
    auto missing_parser = task_request(source.id);
    missing_parser.parser_type.clear();
    cases.emplace_back("parser_type", std::move(missing_parser));

    for (const auto& [name, request] : cases) {
        SCOPED_TRACE(name);
        const auto result = service_.create_task(request);
        EXPECT_FALSE(result.status.ok);
        EXPECT_TRUE(result.id.empty());
    }
}

TEST_F(ConfigServiceTest, RejectsMissingAndCrossNodeDataSources) {
    auto missing_source = task_request("missing");
    const auto missing_result = service_.create_task(missing_source);

    const auto registration = node_service_.register_node(
        {"node-b", "Node B", "0.1.0"});
    ASSERT_TRUE(registration.ok) << registration.message;
    const auto source = service_.create_data_source(data_source_request());
    ASSERT_TRUE(source.status.ok) << source.status.message;
    const auto cross_node_result =
        service_.create_task(task_request(source.id, "node-b"));

    EXPECT_FALSE(missing_result.status.ok);
    EXPECT_FALSE(cross_node_result.status.ok);
    EXPECT_TRUE(missing_result.id.empty());
    EXPECT_TRUE(cross_node_result.id.empty());
}

TEST_F(ConfigServiceTest, ReturnsOnlyEnabledTasksForNode) {
    const auto source = service_.create_data_source(data_source_request());
    ASSERT_TRUE(source.status.ok) << source.status.message;

    auto enabled_request = task_request(source.id);
    const auto enabled = service_.create_task(enabled_request);
    ASSERT_TRUE(enabled.status.ok) << enabled.status.message;
    auto disabled_request = task_request(source.id);
    disabled_request.name = "disabled task";
    disabled_request.enabled = false;
    const auto disabled = service_.create_task(disabled_request);
    ASSERT_TRUE(disabled.status.ok) << disabled.status.message;

    const auto tasks = service_.find_enabled_tasks("node-a");

    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().id, enabled.id);
    EXPECT_EQ(tasks.front().name, enabled_request.name);
    EXPECT_TRUE(tasks.front().enabled);
    EXPECT_TRUE(service_.find_enabled_tasks("").empty());
}

}  // namespace
