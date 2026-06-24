#pragma once

#include "labbridge/server/alert_repository.h"
#include "labbridge/server/sql_session.h"

namespace labbridge::server {

class PostgresAlertRepository final : public IAlertRepository {
public:
    explicit PostgresAlertRepository(ISqlSession& session);

    std::string create(AlertRecord alert) override;
    std::optional<AlertRecord> find_by_id(const std::string& alert_id) const override;
    std::vector<AlertRecord> find_by_node(const std::string& node_code) const override;
    std::vector<AlertRecord> find_by_task_run(const std::string& task_run_id) const override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
