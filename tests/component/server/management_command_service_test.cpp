#include "labbridge/server/application/management_command_service.h"
#include "labbridge/server/application/node_service.h"
#include "labbridge/server/application/config_service.h"
#include "support/server/in_memory_repositories.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using labbridge::core::StatusCode;
using labbridge::server::ManagementTaskCreateRequest;

class ManagementCommandServiceTest : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(node_service_.register_node(
            {"node-a", "Node A", "0.1.0"}).ok);
    }

    std::string create_source(bool enabled = true) {
        const auto result = service_.create_data_source({
            "node-a",
            labbridge::core::SourceType::LocalDirectory,
            "Instrument inbox",
            R"({"root_path":"/srv/inbox","extension":".csv"})",
            enabled,
        });
        EXPECT_TRUE(result.status.ok) << result.status.message;
        return result.id;
    }

    std::string create_rule(const std::string& type,
                            bool enabled = true) {
        const auto result = service_.create_qc_rule({
            type + " rule",
            type,
            "{}",
            enabled,
        });
        EXPECT_TRUE(result.status.ok) << result.status.message;
        return result.id;
    }

    ManagementTaskCreateRequest task_request(
        const std::string& source_id,
        std::vector<std::string> rule_ids) const {
        return {
            "node-a",
            source_id,
            "Minute CSV import",
            "local_file_import",
            "*/5 * * * *",
            "csv_observation",
            "default",
            std::move(rule_ids),
            true,
        };
    }

    labbridge::server::InMemoryNodeRepository nodes_;
    labbridge::server::InMemoryConfigRepository configs_;
    labbridge::server::InMemoryQcRepository qc_;
    labbridge::server::NodeService node_service_{nodes_};
    labbridge::server::ManagementCommandService service_{
        nodes_, configs_, qc_};
};

TEST_F(ManagementCommandServiceTest,
       CreatesExecutableTaskAndPreservesRequestedQcOrder) {
    const auto source_id = create_source();
    const auto required_id = create_rule("required_fields");
    const auto timestamp_id = create_rule("basic_timestamp_format");

    const auto result = service_.create_task(
        task_request(source_id, {timestamp_id, required_id}));

    ASSERT_TRUE(result.status.ok) << result.status.message;
    EXPECT_EQ(configs_.find_task_qc_rule_ids(result.id),
              (std::vector<std::string>{timestamp_id, required_id}));

    labbridge::server::ConfigService projection_service{nodes_, configs_};
    const auto projection =
        projection_service.find_executable_config("node-a");
    ASSERT_EQ(projection.tasks.size(), 1U);
    EXPECT_EQ(projection.tasks.front().qc_rule_ids,
              (std::vector<std::string>{timestamp_id, required_id}));
}

TEST_F(ManagementCommandServiceTest,
       RejectsUnsupportedShapesAndNonExecutableDependencies) {
    const auto invalid_source = service_.create_data_source({
        "node-a",
        labbridge::core::SourceType::LocalDirectory,
        "Broken source",
        R"({"root_path":"/srv/inbox","extension":"csv"})",
        true,
    });
    EXPECT_FALSE(invalid_source.status.ok);
    EXPECT_EQ(invalid_source.status.code, StatusCode::InvalidArgument);

    const auto unsupported_rule = service_.create_qc_rule({
        "Future rule", "range_check", "{}", true});
    EXPECT_FALSE(unsupported_rule.status.ok);
    EXPECT_EQ(unsupported_rule.status.code, StatusCode::InvalidArgument);

    const auto source_id = create_source();
    const auto disabled_rule = create_rule("required_fields", false);
    const auto disabled_dependency = service_.create_task(
        task_request(source_id, {disabled_rule}));
    EXPECT_FALSE(disabled_dependency.status.ok);
    EXPECT_EQ(disabled_dependency.status.code, StatusCode::Conflict);

    const auto enabled_rule = create_rule("required_fields");
    const auto duplicate = service_.create_task(
        task_request(source_id, {enabled_rule, enabled_rule}));
    EXPECT_FALSE(duplicate.status.ok);
    EXPECT_EQ(duplicate.status.code, StatusCode::Conflict);
}

TEST_F(ManagementCommandServiceTest,
       DisableIsImmediateAndEnableRevalidatesDependencies) {
    const auto source_id = create_source();
    const auto rule_id = create_rule("required_fields");
    const auto task = service_.create_task(
        task_request(source_id, {rule_id}));
    ASSERT_TRUE(task.status.ok) << task.status.message;

    ASSERT_TRUE(service_.set_task_enabled(task.id, false).status.ok);
    labbridge::server::ConfigService projection_service{nodes_, configs_};
    EXPECT_TRUE(
        projection_service.find_executable_config("node-a").tasks.empty());

    qc_.create_rule({
        rule_id,
        "required_fields rule",
        "required_fields",
        "{}",
        false,
    });
    const auto rejected_enable = service_.set_task_enabled(task.id, true);
    EXPECT_FALSE(rejected_enable.status.ok);
    EXPECT_EQ(rejected_enable.status.code, StatusCode::Conflict);
    ASSERT_TRUE(configs_.find_task(task.id).has_value());
    EXPECT_FALSE(configs_.find_task(task.id)->enabled);

    qc_.create_rule({
        rule_id,
        "required_fields rule",
        "required_fields",
        "{}",
        true,
    });
    ASSERT_TRUE(service_.set_task_enabled(task.id, true).status.ok);
    EXPECT_EQ(
        projection_service.find_executable_config("node-a").tasks.size(),
        1U);
}

}  // namespace
