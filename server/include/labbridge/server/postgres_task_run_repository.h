#pragma once

#include "labbridge/server/sql_session.h"
#include "labbridge/server/task_run_repository.h"

namespace labbridge::server {

class PostgresTaskRunRepository final : public ITaskRunRepository {
public:
    explicit PostgresTaskRunRepository(ISqlSession& session);

    std::string create(TaskRunRecord task_run) override;
    std::optional<TaskRunRecord> find_by_id(const std::string& task_run_id) const override;
    void finish(TaskRunRecord task_run) override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
