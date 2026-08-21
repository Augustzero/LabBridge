#pragma once

#include "labbridge/core/models.h"

#include <optional>
#include <string>
#include <vector>

namespace labbridge::server {

struct DataSourceRecord {
    std::string id;
    std::string node_code;
    labbridge::core::SourceType source_type{labbridge::core::SourceType::LocalDirectory};
    std::string name;
    std::string config_json;
    bool enabled{true};
    std::string created_at;
    std::string updated_at;
};

struct TaskRecord {
    std::string id;
    std::string node_code;
    std::string data_source_id;
    std::string name;
    std::string task_type;
    std::string schedule_expr;
    std::string parser_type;
    std::string qc_profile;
    bool enabled{true};
    std::vector<std::string> qc_rule_ids;
    std::string created_at;
    std::string updated_at;
};

struct TaskQcRuleBinding {
    std::string task_id;
    std::string qc_rule_id;
    std::string rule_type;
    std::string name;
    std::string rule_config_json;
    int sort_order{0};
};

class IConfigRepository {
public:
    virtual ~IConfigRepository() = default;

    virtual std::string create_data_source(DataSourceRecord data_source) = 0;
    virtual std::optional<DataSourceRecord> find_data_source(const std::string& data_source_id) const = 0;
    virtual std::string create_task(TaskRecord task) = 0;
    virtual std::optional<TaskRecord> find_task(const std::string& task_id) const = 0;
    virtual std::vector<TaskRecord> find_enabled_tasks_by_node(const std::string& node_code) const = 0;
    virtual std::vector<DataSourceRecord> find_enabled_data_sources_by_node(
        const std::string& node_code) const = 0;
    virtual std::vector<TaskQcRuleBinding> find_enabled_task_qc_rules_by_node(
        const std::string& node_code) const = 0;
    virtual void bind_task_qc_rule(const std::string& task_id,
                                   const std::string& qc_rule_id,
                                   int sort_order) = 0;
    virtual std::vector<std::string> find_task_qc_rule_ids(
        const std::string& task_id) const = 0;
    virtual void set_task_enabled(const std::string& task_id, bool enabled) = 0;
};

}  // namespace labbridge::server
