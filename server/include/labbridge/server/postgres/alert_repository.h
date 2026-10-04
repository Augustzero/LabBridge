#pragma once

#include "labbridge/server/repositories/alert_repository.h"
#include "labbridge/server/postgres/sql_session.h"

namespace labbridge::server {

class PostgresAlertRepository final : public IAlertRepository {
public:
    explicit PostgresAlertRepository(ISqlSession& session);

    std::string create(AlertRecord alert) override;
    std::vector<AlertRecord> find_by_node(const std::string& node_code) const override;
    std::vector<AlertRecord> find_by_task_run(const std::string& task_run_id) const override;
    std::optional<AlertRecord> lock_by_id(const std::string& alert_id) override;
    std::optional<AlertRecord> save_acknowledged(const std::string& alert_id) override;
    std::optional<AlertRecord> save_closed(const std::string& alert_id) override;

private:
    // 事务内已持锁后的普通重读，save_* 写入完成后用它取完整行。
    std::optional<AlertRecord> read_by_id(const std::string& alert_id) const;

    ISqlSession& session_;
};

}  // namespace labbridge::server
