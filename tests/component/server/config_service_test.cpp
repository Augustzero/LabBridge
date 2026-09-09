#include "support/server/in_memory_repositories.h"
#include "support/server/test_config_seed.h"
#include "labbridge/server/application/config_service.h"

#include <gtest/gtest.h>

#include <string>

TEST(ConfigServiceTest, ReturnsOnlyEnabledTasksForNode) {
    labbridge::server::InMemoryConfigRepository configs_;
    labbridge::server::ConfigService service_{configs_};

    const auto enabled_id = labbridge::server::test_support::create_csv_task(
        configs_, "node-a", "1", "collect observations");
    const auto disabled_id = labbridge::server::test_support::create_csv_task(
        configs_, "node-a", "1", "disabled task", false);
    ASSERT_FALSE(enabled_id.empty());
    ASSERT_FALSE(disabled_id.empty());

    const auto tasks = service_.find_enabled_tasks("node-a");

    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().id, enabled_id);
    EXPECT_EQ(tasks.front().name, "collect observations");
    EXPECT_TRUE(tasks.front().enabled);
    EXPECT_TRUE(service_.find_enabled_tasks("").empty());
}
