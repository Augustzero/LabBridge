#pragma once

#include "labbridge/server/repositories/config_repository.h"
#include "labbridge/server/postgres/sql_session.h"

namespace labbridge::server {

class PostgresConfigRepository final : public IConfigRepository {
public:
    explicit PostgresConfigRepository(ISqlSession& session);

    std::string create_data_source(DataSourceRecord data_source) override;
    std::optional<DataSourceRecord> find_data_source(const std::string& data_source_id) const override;
    std::string create_task(TaskRecord task) override;
    std::optional<TaskRecord> find_task(const std::string& task_id) const override;
    std::vector<TaskRecord> find_enabled_tasks_by_node(const std::string& node_code) const override;
    std::vector<DataSourceRecord> find_enabled_data_sources_by_node(
        const std::string& node_code) const override;
    std::vector<TaskQcRuleBinding> find_enabled_task_qc_rules_by_node(
        const std::string& node_code) const override;
    void bind_task_qc_rule(const std::string& task_id,
                           const std::string& qc_rule_id,
                           int sort_order) override;
    std::vector<std::string> find_task_qc_rule_ids(
        const std::string& task_id) const override;
    void set_task_enabled(const std::string& task_id,
                          bool enabled) override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
