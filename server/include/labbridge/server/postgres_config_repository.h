#pragma once

#include "labbridge/server/config_repository.h"
#include "labbridge/server/sql_session.h"

namespace labbridge::server {

class PostgresConfigRepository final : public IConfigRepository {
public:
    explicit PostgresConfigRepository(ISqlSession& session);

    std::string create_data_source(DataSourceRecord data_source) override;
    std::optional<DataSourceRecord> find_data_source(const std::string& data_source_id) const override;
    std::string create_task(TaskRecord task) override;
    std::vector<TaskRecord> find_enabled_tasks_by_node(const std::string& node_code) const override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
