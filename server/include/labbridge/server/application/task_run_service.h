#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/repositories/config_repository.h"
#include "labbridge/server/repositories/node_repository.h"
#include "labbridge/server/repositories/qc_repository.h"
#include "labbridge/server/repositories/result_repository.h"
#include "labbridge/server/repositories/task_run_repository.h"

#include <optional>
#include <string>

namespace labbridge::server {

struct StartTaskRunRequest {
    std::string node_code;
    std::string task_id;
    std::string started_at;
    std::string trigger_type{"scheduled"};
    std::string execution_key;
    std::string scheduled_for;
};

struct FinishTaskRunRequest {
    std::string task_run_id;
    labbridge::core::TaskRunStatus status{labbridge::core::TaskRunStatus::Succeeded};
    std::string finished_at;
    int items_total{0};
    int items_success{0};
    int items_failed{0};
    std::string error_summary;
    // 终态报告的失败清单；has=false 时保持库内 NULL（旧请求语义）。
    bool has_failed_files{false};
    std::vector<TaskRunFailedFile> failed_files;
};

struct TaskRunCreateResult {
    labbridge::core::Status status;
    std::string id;
    bool replayed{false};
    // 本次 start 之后运行的状态（running / failed / succeeded），
    // Agent 据此决定继续执行还是收尾空作业。
    std::string run_status;
};

// 管理端人工受理（trigger / retry）的统一结果。
struct ManualTaskRunResult {
    labbridge::core::Status status;
    std::string task_run_id;
    std::string run_status;
    bool replayed{false};
};

class TaskRunService {
public:
    TaskRunService(IConfigRepository& config_repository,
                   ITaskRunRepository& task_run_repository,
                   INodeRepository& node_repository,
                   IQcRepository& qc_repository,
                   IResultRepository& result_repository);

    TaskRunCreateResult start(const StartTaskRunRequest& request);
    labbridge::core::Status finish(const FinishTaskRunRequest& request);
    std::optional<TaskRunRecord> find_run(const std::string& task_run_id) const;
    // 读取 retry 运行的固定输入快照；nullopt 表示普通运行。
    std::optional<std::vector<TaskRunRetryFile>> find_retry_files(
        const std::string& task_run_id) const;

    // 对已启用任务提交一次人工执行（manual）。
    ManualTaskRunResult request_manual_run(const std::string& task_id,
                                           const std::string& idempotency_key);
    // 对终态 failed 且有失败清单的运行创建固定输入的重试运行（retry）。
    ManualTaskRunResult request_retry_run(const std::string& task_run_id,
                                          const std::string& idempotency_key);

private:
    TaskRunCreateResult start_manual(const StartTaskRunRequest& request);
    // trigger / retry 共用的受理骨架：重放判定 → 任务可执行 → pending 冲突 → 落库。
    ManualTaskRunResult apply_manual_run(const std::string& trigger_type,
                                         const std::string& task_id,
                                         const std::string& retry_of_run_id,
                                         const std::string& idempotency_key,
                                         std::vector<TaskRunRetryFile> retry_files);
    // 按原失败清单解析归档身份，固定重试输入；拒绝时返回失败状态。
    labbridge::core::Status resolve_retry_files(
        const TaskRunRecord& parent_run,
        const std::vector<TaskRunFailedFile>& failed_files,
        std::vector<TaskRunRetryFile>& retry_files) const;
    ManualTaskRunResult replayed_result(const TaskRunRecord& run) const;
    TaskRunCreateResult replay_result(const TaskRunRecord& run) const;

    IConfigRepository& config_repository_;
    ITaskRunRepository& task_run_repository_;
    INodeRepository& node_repository_;
    IQcRepository& qc_repository_;
    IResultRepository& result_repository_;
};

}  // namespace labbridge::server
