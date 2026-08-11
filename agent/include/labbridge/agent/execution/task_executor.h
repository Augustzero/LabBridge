#pragma once

#include "labbridge/agent/execution/local_archive_store.h"
#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/agent/scheduler/task_scheduler.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace labbridge::agent {

class TaskExecutor final : public ITaskExecutor {
public:
    using NowFunction =
        std::function<std::chrono::system_clock::time_point()>;

    TaskExecutor(ITaskExecutionClient& client,
                 labbridge::core::fs::path work_dir,
                 std::vector<labbridge::core::fs::path> allowed_local_roots,
                 std::size_t fingerprint_capacity = 1024);
    TaskExecutor(ITaskExecutionClient& client,
                 labbridge::core::fs::path work_dir,
                 std::vector<labbridge::core::fs::path> allowed_local_roots,
                 NowFunction now,
                 std::size_t fingerprint_capacity = 1024);

    void execute(ScheduledTaskExecution execution) override;
    void forget_task(const std::string& task_id);

private:
    bool was_processed(const std::string& task_id,
                       const std::string& fingerprint) const;
    void remember_processed(const std::string& task_id,
                            std::string fingerprint);

    ITaskExecutionClient& client_;
    LocalArchiveStore archive_store_;
    std::vector<labbridge::core::fs::path> allowed_local_roots_;
    NowFunction now_;
    std::size_t fingerprint_capacity_;
    std::unordered_map<std::string, std::deque<std::string>>
        processed_fingerprints_;
};

}  // namespace labbridge::agent
