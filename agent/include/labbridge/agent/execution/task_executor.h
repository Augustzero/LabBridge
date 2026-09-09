#pragma once

#include "labbridge/agent/execution/local_archive_store.h"
#include "labbridge/agent/execution/reliable_execution_store.h"
#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/agent/scheduler/task_scheduler.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace labbridge::agent {

class TaskExecutor final : public ITaskExecutor {
public:
    using NowFunction =
        std::function<std::chrono::system_clock::time_point()>;

    TaskExecutor(ITaskExecutionClient& client,
                 IReliableExecutionStore& queue_store,
                 labbridge::core::fs::path work_dir,
                 std::vector<labbridge::core::fs::path> allowed_local_roots,
                 NowFunction now);

    void recover_pending_jobs() override;

    void execute(ScheduledTaskExecution execution) override;
    void request_stop() noexcept override;

private:
    void run_reliable_job(RecoveredJob job);
    // collecting 阶段推进（文件规划 + 归档 + manifest 落库），返回重载后的作业；
    // 异常抛出时入参 job 保持原状，供失败折叠路径使用。
    RecoveredJob run_collecting_stage(const RecoveredJob& job) const;
    // 采集类失败折叠为终态 failed report（对齐旧直连路径语义），返回重载后的作业。
    RecoveredJob save_collection_failure_report(const RecoveredJob& job,
                                                const std::string& error) const;
    RecoveredJob load_job(const std::string& execution_key) const;

    ITaskExecutionClient& client_;
    IReliableExecutionStore& queue_store_;
    LocalArchiveStore archive_store_;
    std::vector<labbridge::core::fs::path> allowed_local_roots_;
    NowFunction now_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace labbridge::agent
