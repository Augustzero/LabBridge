#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/core/models.h"

#include <optional>
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
    // 逻辑来源路径：报告和指纹用它表达“这是哪个源文件”。
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
    // 实际读取路径：普通采集与 source_path 相同；归档重放是旧归档路径。
    // 空串按旧记录语义回退到 source_path。
    std::string input_path;
    // 归档或读取失败的原因；非空表示该目标已失败，恢复时跳过。
    std::string error_detail;
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
    // retry 作业的固定目标；manual / scheduled 为空。
    std::vector<RetryFileInput> retry_files;
    // 已持久化的文件失败清单；has=false 表示尚未生成计划。
    bool has_file_failures{false};
    std::vector<TaskRunReportFailedFile> file_failures;
};

class IReliableExecutionStore {
public:
    virtual ~IReliableExecutionStore() = default;

    // retry_files 非空时作业按固定目标执行，不扫描目录；
    // 与首次 start 投递同一事务保存。
    virtual bool begin_job(
        const labbridge::core::TaskConfig& task,
        const StartTaskRunRequest& request,
        const std::vector<RetryFileInput>& retry_files) = 0;
    virtual std::vector<RecoveredJob> recover_jobs() const = 0;
    virtual RecoveredJob load_job(
        const std::string& execution_key) const = 0;
    // 含 requires_attention 的存在性查询；人工候选按执行键去重用。
    virtual std::optional<RecoveredJob> find_job(
        const std::string& execution_key) const = 0;
    virtual void accept_start(const std::string& execution_key,
                              const std::string& task_run_id) = 0;
    // 文件计划与当时的失败清单同一事务提交：计划存在即代表选文件完成，
    // 空失败数组也落库（区别于旧作业的 NULL）。
    virtual void save_file_plan(
        const std::string& execution_key,
        const std::vector<PendingFilePlan>& files,
        const std::vector<TaskRunReportFailedFile>& failures) = 0;
    // 独立更新失败清单，只用于恢复时补齐旧版本留下的半写状态；
    // 正常执行路径的清单更新走 mark_file_failed 的事务。
    virtual void save_file_failures(
        const std::string& execution_key,
        const std::vector<TaskRunReportFailedFile>& failures) = 0;
    // 计划内某个目标的归档/读取失败：error_detail 与更新后的完整失败清单
    // 同一事务提交，中途崩溃要么都在要么都不在。恢复时跳过已记失败的项，
    // 不在同一作业里重试它们。
    virtual void mark_file_failed(const std::string& execution_key,
                                  int ordinal,
                                  const std::string& error_detail,
                                  const std::vector<TaskRunReportFailedFile>&
                                      failures) = 0;
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
    // 收尾中心已终态、本地还没生成文件计划的空作业：
    // 只允许 start_pending 且无计划行，直接删除，不写 processed 指纹。
    virtual void discard_start_pending_job(const std::string& execution_key) = 0;
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
    // 只查“在途占用”（排队/attention 作业的计划行），不看历史 processed；
    // retry 明确选中的历史失败输入允许再次处理。
    virtual bool is_file_in_flight(const std::string& task_id,
                                   const std::string& fingerprint) const = 0;
};

}  // namespace labbridge::agent
