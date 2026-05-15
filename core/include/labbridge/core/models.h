#pragma once

#include <string>
#include <vector>

namespace labbridge::core {

enum class SourceType {
    LocalDirectory,
    Ftp,
    Oracle,
};

enum class TaskRunStatus {
    Pending,
    Running,
    Succeeded,
    Failed,
};

struct NodeInfo {
    std::string node_code;
    std::string name;
    std::string agent_version;
};

enum class NodeStatus {
    Online,
    Offline,
};

struct NodeHeartbeat {
    std::string node_code;
    std::string agent_version;
    std::string reported_at;
};

struct TaskRunSummary {
    std::string task_id;
    std::string node_code;
    TaskRunStatus status{TaskRunStatus::Pending};
    int items_total{0};
    int items_success{0};
    int items_failed{0};
    std::string error_summary;
};

struct DataSourceConfig {
    std::string id;
    SourceType type{SourceType::LocalDirectory};
    std::string name;
    std::string config_json;
};

struct TaskConfig {
    std::string id;
    std::string node_code;
    std::string data_source_id;
    std::string name;
    std::string schedule_expr;
    std::string parser_type;
    std::vector<std::string> qc_rules;
};

struct ParsedRecord {
    std::string station_code;
    std::string device_code;
    std::string record_time;
    std::string payload_json;
};

}  // namespace labbridge::core
