#include "support/server/in_memory_repositories.h"
#include "labbridge/agent/qc/basic_qc_rules.h"
#include "labbridge/agent/parsers/csv_parser.h"
#include "labbridge/agent/collectors/local_dir_collector.h"
#include "labbridge/core/version.h"
#include "labbridge/server/application/node_service.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

TEST(LocalCsvPipelineTest, CollectsParsesAndChecksSampleCsv) {
    labbridge::server::InMemoryNodeRepository repository;
    labbridge::server::NodeService node_service(repository);

    const auto register_status = node_service.register_node({
        "lab-node-001",
        "default-lab-node",
        labbridge::core::kVersion,
    });
    EXPECT_TRUE(register_status.ok);

    const auto heartbeat_status = node_service.accept_heartbeat({
        "lab-node-001",
        labbridge::core::kVersion,
        "2026-05-09 10:00:00",
    });
    EXPECT_TRUE(heartbeat_status.ok);

    const auto node = node_service.find_node("lab-node-001");
    EXPECT_TRUE(node.has_value());
    EXPECT_TRUE(node->status == labbridge::core::NodeStatus::Online);

    labbridge::agent::LocalDirCollector collector{"tests/fixtures/agent", ".csv"};
    const auto collect_result = collector.collect({
        "task-local-csv-001",
        "lab-node-001",
        "{}",
    });
    EXPECT_TRUE(collect_result.status.ok);
    const auto sample = std::find_if(
        collect_result.items.begin(), collect_result.items.end(),
        [](const auto& item) {
            return item.original_name == "sample_observation.csv";
        });
    EXPECT_TRUE(sample != collect_result.items.end());
    EXPECT_TRUE(!collect_result.items.empty());

    labbridge::agent::CsvObservationParser parser;
    const auto parse_result = parser.parse({
        "run-001",
        "raw-001",
        sample->local_path,
    });
    EXPECT_TRUE(parse_result.status.ok);
    EXPECT_TRUE(parse_result.records.size() == 2);

    labbridge::agent::RequiredFieldsRule required_fields_rule;
    labbridge::agent::BasicTimestampRule timestamp_rule;

    for (const auto& record : parse_result.records) {
        const auto required_result = required_fields_rule.check(record);
        const auto timestamp_result = timestamp_rule.check(record);
        EXPECT_TRUE(required_result.level == labbridge::agent::QcLevel::Pass);
        EXPECT_TRUE(timestamp_result.level == labbridge::agent::QcLevel::Pass);
    }

}
