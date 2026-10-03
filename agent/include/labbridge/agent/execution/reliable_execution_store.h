#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/core/models.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace labbridge::agent {

// 可靠队列存储的失败（SQLite I/O、约束违例、schema 不兼容）。
// 按约定传播到进程边界非零退出，不在作业循环内吞掉。
class AgentQueueError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct PendingFilePlan {
    int ordinal{0};
    std::string source_path;
    std::string original_name;
    std::string source_mtime;
    long long size_bytes{0};
    std::string file_hash;
    std::string fingerprint;
    std::string archive_path;
    std::string archive_state{"archive_planned"};
    std::string raw_file_id;
    bool parsed_without_errors{false};
};

struct RecoveredJob {
    std::string execution_key;
    labbridge::core::TaskConfig task;
    StartTaskRunRequest start_request;
    std::string stage;
    std::string task_run_id;
    std::vector<PendingFilePlan> files;
    RawFileManifestRequest manifest_request;
    TaskRunReportRequest report_request;
};

class IReliableExecutionStore {
public:
    virtual ~IReliableExecutionStore() = default;

    virtual bool begin_job(const labbridge::core::TaskConfig& task,
                           const StartTaskRunRequest& request) = 0;
    virtual std::vector<RecoveredJob> recover_jobs() const = 0;
    virtual RecoveredJob load_job(
        const std::string& execution_key) const = 0;
    virtual void accept_start(const std::string& execution_key,
                              const std::string& task_run_id) = 0;
    virtual void save_file_plan(
        const std::string& execution_key,
        const std::vector<PendingFilePlan>& files) = 0;
    virtual void mark_file_archived(const std::string& execution_key,
                                    int ordinal) = 0;
    virtual void save_manifest(const std::string& execution_key,
                               const RawFileManifestRequest& request) = 0;
    virtual void accept_manifest(
        const std::string& execution_key,
        const std::vector<std::string>& raw_file_ids) = 0;
    virtual void save_report(
        const std::string& execution_key,
        const TaskRunReportRequest& request,
        const std::vector<bool>& parsed_without_errors) = 0;
    virtual void complete_job(const std::string& execution_key) = 0;
    // 归档冲突、请求体超限等不可自动重试的作业级故障：作业停留
    // requires_attention 等待人工处理。error_kind 记录错误种类
    // （如 archive_conflict、payload_too_large），恢复阶段由存储自行保存。
    virtual void mark_requires_attention(const std::string& execution_key,
                                         const std::string& error_kind,
                                         const std::string& reason) = 0;
    virtual bool has_capacity() const = 0;
    // 文件是否已被处理过或正被某个排队作业（含 requires_attention）占用，
    // 用于新计划去重；按 task_id 隔离，不同任务互不影响。
    virtual bool is_file_occupied(const std::string& task_id,
                                  const std::string& fingerprint) const = 0;
};

}  // namespace labbridge::agent
