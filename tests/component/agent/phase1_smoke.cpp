#include "support/server/in_memory_repositories.h"
#include "labbridge/agent/qc/basic_qc_rules.h"
#include "labbridge/agent/parsers/csv_parser.h"
#include "labbridge/agent/collectors/local_dir_collector.h"
#include "labbridge/core/version.h"
#include "labbridge/server/application/node_service.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    labbridge::server::InMemoryNodeRepository repository;
    labbridge::server::NodeService node_service(repository);

    const auto register_status = node_service.register_node({
        "lab-node-001",
        "default-lab-node",
        labbridge::core::kVersion,
    });
    assert(register_status.ok);

    const auto heartbeat_status = node_service.accept_heartbeat({
        "lab-node-001",
        labbridge::core::kVersion,
        "2026-05-09 10:00:00",
    });
    assert(heartbeat_status.ok);

    const auto node = node_service.find_node("lab-node-001");
    assert(node.has_value());
    assert(node->status == labbridge::core::NodeStatus::Online);

    labbridge::agent::LocalDirCollector collector{"tests/fixtures/agent", ".csv"};
    const auto collect_result = collector.collect({
        "task-local-csv-001",
        "lab-node-001",
        "{}",
    });
    assert(collect_result.status.ok);
    assert(!collect_result.items.empty());

    labbridge::agent::CsvObservationParser parser;
    const auto parse_result = parser.parse({
        "run-001",
        "raw-001",
        collect_result.items.front().local_path,
    });
    assert(parse_result.status.ok);
    assert(parse_result.records.size() == 2);

    labbridge::agent::RequiredFieldsRule required_fields_rule;
    labbridge::agent::BasicTimestampRule timestamp_rule;

    for (const auto& record : parse_result.records) {
        const auto required_result = required_fields_rule.check(record);
        const auto timestamp_result = timestamp_rule.check(record);
        assert(required_result.level == labbridge::agent::QcLevel::Pass);
        assert(timestamp_result.level == labbridge::agent::QcLevel::Pass);
    }

    return 0;
}

