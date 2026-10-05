#pragma once

#include "labbridge/server/postgres/sql_session.h"
#include "labbridge/server/repositories/task_run_repository.h"

namespace labbridge::server {

class PostgresTaskRunRepository final : public ITaskRunRepository {
public:
    explicit PostgresTaskRunRepository(ISqlSession& session);

    std::string create(TaskRunRecord task_run) override;
    ScheduledTaskRunStart create_or_find_scheduled(
        TaskRunRecord task_run) override;
    std::optional<ManualTaskRunStart> create_or_find_manual(
        ManualTaskRunRequest request) override;
    std::optional<std::vector<TaskRunFailedFile>> find_failed_files(
        const std::string& task_run_id) const override;
    std::optional<std::vector<TaskRunRetryFile>> find_retry_files(
        const std::string& task_run_id) const override;
    std::optional<TaskRunRecord> find_by_id(const std::string& task_run_id) const override;
    void finish(TaskRunRecord task_run) override;
    std::optional<TaskRunRecord> find_by_execution_key(
        const std::string& execution_key) const override;
    bool lock_task(const std::string& task_id) override;
    std::optional<TaskRunRecord> lock_by_id(
        const std::string& task_run_id) override;
    std::optional<TaskRunRecord> find_pending_manual_by_task(
        const std::string& task_id) const override;
    void fail_pending_manual_runs(
        const std::string& task_id, const std::string& error_summary) override;
    void mark_manual_running(
        const std::string& task_run_id, const std::string& started_at) override;
    std::vector<PendingExecutionRecord> find_pending_executions_by_node(
        const std::string& node_code) const override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
