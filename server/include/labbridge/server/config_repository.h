#pragma once

#include "labbridge/core/models.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace labbridge::server {

struct DataSourceRecord {
    std::string id;
    std::string node_code;
    labbridge::core::SourceType source_type{labbridge::core::SourceType::LocalDirectory};
    std::string name;
    std::string config_json;
    bool enabled{true};
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
};

class IConfigRepository {
public:
    virtual ~IConfigRepository() = default;

    virtual std::string create_data_source(DataSourceRecord data_source) = 0;
    virtual std::optional<DataSourceRecord> find_data_source(const std::string& data_source_id) const = 0;
    virtual std::string create_task(TaskRecord task) = 0;
    virtual std::optional<TaskRecord> find_task(const std::string& task_id) const = 0;
    virtual std::vector<TaskRecord> find_enabled_tasks_by_node(const std::string& node_code) const = 0;
};

class InMemoryConfigRepository final : public IConfigRepository {
public:
    std::string create_data_source(DataSourceRecord data_source) override;
    std::optional<DataSourceRecord> find_data_source(const std::string& data_source_id) const override;
    std::string create_task(TaskRecord task) override;
    std::optional<TaskRecord> find_task(const std::string& task_id) const override;
    std::vector<TaskRecord> find_enabled_tasks_by_node(const std::string& node_code) const override;

private:
    int next_data_source_id_{1};
    int next_task_id_{1};
    std::unordered_map<std::string, DataSourceRecord> data_sources_;
    std::unordered_map<std::string, TaskRecord> tasks_;
};

}  // namespace labbridge::server
