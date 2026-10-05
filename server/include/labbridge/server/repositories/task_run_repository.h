#pragma once

#include "labbridge/core/models.h"

#include <optional>
#include <string>
#include <vector>

namespace labbridge::server {

// 终态报告里的文件级失败：路径是确切文件，不是目录或通配符。
// archive_raw_file_id 为空表示从未取得有效归档（只能定点补采）。
struct TaskRunFailedFile {
    std::string source_path;
    std::string original_name;
    std::string stage;
    std::string message;
    std::string archive_raw_file_id;
};

// 创建 retry 时固定的输入快照。input_type 为 archive 时字段齐全，
// 为 source 时只有确切路径和文件名，内容在实际读取时才固定。
struct TaskRunRetryFile {
    std::string input_type;
    std::string source_path;
    std::string original_name;
    std::string archive_raw_file_id;
    std::string storage_path;
    long long size_bytes{0};
    std::string file_hash;
    std::string source_mtime;
};

// 配置轮询下发给 Agent 的人工待执行项。
struct PendingExecutionRecord {
    std::string task_run_id;
    std::string task_id;
    std::string execution_key;
    std::string trigger_type;
    std::string requested_at;
    std::vector<TaskRunRetryFile> retry_files;
};

struct TaskRunRecord {
    std::string id;
    std::string task_id;
    std::string node_code;
    labbridge::core::TaskRunStatus status{labbridge::core::TaskRunStatus::Pending};
    std::string started_at;
    std::string finished_at;
    int items_total{0};
    int items_success{0};
    int items_failed{0};
    std::string error_summary;
    std::string trigger_type{"scheduled"};
    std::string execution_key;
    std::string scheduled_for;
    std::string requested_at;
    std::string retry_of_run_id;
    // 终态写入时的失败清单 JSON；空串表示写 NULL（保留旧值）。
    std::string failed_files_json;
    // -1 表示库里是 NULL（旧报告没有失败清单），>= 0 是清单条数。
    int failed_file_count{-1};
};

struct ScheduledTaskRunStart {
    TaskRunRecord task_run;
    bool created{false};
};

// 人工受理请求：trigger/retry 两个入口共用的落库参数。
struct ManualTaskRunRequest {
    std::string task_id;
    std::string node_code;
    std::string trigger_type;
    std::string execution_key;
    std::string retry_of_run_id;
    std::vector<TaskRunRetryFile> retry_files;
};

struct ManualTaskRunStart {
    TaskRunRecord task_run;
    bool created{false};
};

class ITaskRunRepository {
public:
    virtual ~ITaskRunRepository() = default;

    virtual std::string create(TaskRunRecord task_run) = 0;
    virtual ScheduledTaskRunStart create_or_find_scheduled(
        TaskRunRecord task_run) = 0;
    // 受理人工请求；插入被并发请求抢先（同任务另一 pending 键）时返回
    // nullopt，由调用方按 pending 冲突处理。
    virtual std::optional<ManualTaskRunStart> create_or_find_manual(
        ManualTaskRunRequest request) = 0;
    // 读取终态报告写入的失败清单；nullopt 表示 NULL（旧报告）。
    virtual std::optional<std::vector<TaskRunFailedFile>> find_failed_files(
        const std::string& task_run_id) const = 0;
    // 读取创建 retry 时固定的输入快照；nullopt 表示普通运行。
    virtual std::optional<std::vector<TaskRunRetryFile>> find_retry_files(
        const std::string& task_run_id) const = 0;
    virtual std::optional<TaskRunRecord> find_by_id(
        const std::string& task_run_id) const = 0;
    virtual void finish(TaskRunRecord task_run) = 0;
    // 按执行键反查运行：人工重放判定靠它，键全局随机所以不按节点过滤。
    virtual std::optional<TaskRunRecord> find_by_execution_key(
        const std::string& execution_key) const = 0;
    // “任务行 → 运行行”加锁顺序的第一步，串行化任务禁用与人工受理。
    virtual bool lock_task(const std::string& task_id) = 0;
    virtual std::optional<TaskRunRecord> lock_by_id(
        const std::string& task_run_id) = 0;
    // 同任务已有尚未开始的人工请求时返回它，用于 409 提示。
    virtual std::optional<TaskRunRecord> find_pending_manual_by_task(
        const std::string& task_id) const = 0;
    // 禁用任务时把未开始的人工请求收尾为 failed，空清单表示没有文件级失败。
    virtual void fail_pending_manual_runs(
        const std::string& task_id, const std::string& error_summary) = 0;
    // 供 Agent start 把 pending 原子推进为 running 并写实际开始时间。
    // 调用方必须已持有运行行锁，这里不再复查状态。
    virtual void mark_manual_running(
        const std::string& task_run_id, const std::string& started_at) = 0;
    virtual std::vector<PendingExecutionRecord>
    find_pending_executions_by_node(const std::string& node_code) const = 0;
};

// JSONB 清单与文本互转；存储文本为空串表示 NULL。
// 破损的存储内容属于内部约束违例，直接抛错不上报业务码。
std::string to_failed_files_json(const std::vector<TaskRunFailedFile>& files);
// 解析失败或结构不对时返回 std::nullopt，由调用方决定错误路径。
std::optional<std::vector<TaskRunFailedFile>> parse_failed_files_json(
    const std::string& json);
std::string to_retry_files_json(const std::vector<TaskRunRetryFile>& files);
std::optional<std::vector<TaskRunRetryFile>> parse_retry_files_json(
    const std::string& json);

}  // namespace labbridge::server
