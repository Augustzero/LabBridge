#pragma once

#include "labbridge/core/models.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace labbridge::agent {

enum class TaskExecutionErrorKind { Network, HttpStatus, InvalidResponse, ServerError };

class TaskExecutionClientError : public std::runtime_error {
public:
    TaskExecutionClientError(TaskExecutionErrorKind kind, std::string message,
                             unsigned int http_status = 0);
    TaskExecutionErrorKind kind() const noexcept;
    unsigned int http_status() const noexcept;
    // 瞬时错误统一判定：网络故障，或 408/429/5xx 状态。
    // 400/409/413 这类永久拒绝重试也不会成功，一律返回 false。
    bool is_transient() const noexcept;
    // 认证被拒（401/403）：凭据或节点身份配错了，原地重试只会继续被拒。
    // 调用方应保留现场后停止进程，等修正配置重启再继续。
    bool is_auth_rejection() const noexcept;
private:
    TaskExecutionErrorKind kind_;
    unsigned int http_status_;
};


struct StartTaskRunRequest {
    std::string node_code;
    std::string task_id;
    std::string execution_key;
    std::string scheduled_for;
    std::string started_at;
    std::string trigger_type{"scheduled"};
};

struct StartTaskRunResult {
    std::string task_run_id;
    bool replayed{false};
    // start 之后运行的状态：running 表示继续执行；
    // failed / succeeded 表示中心已把运行收尾（如开始前任务被停用），
    // Agent 只需收尾本地空作业，不能再采集。
    std::string run_status{"running"};
};

struct RawFileManifestEntry {
    std::string original_name;
    std::string file_hash;
    std::string storage_path;
    long long size_bytes{0};
    std::string source_mtime;
    std::string ingest_status{"archived_local"};
};

struct RawFileManifestRequest {
    std::string task_run_id;
    std::string node_code;
    std::string idempotency_key;
    std::vector<RawFileManifestEntry> files;
};

struct RawFileManifestResult {
    std::vector<std::string> raw_file_ids;
    bool replayed{false};
};

struct TaskRunReportQcResult {
    std::string qc_rule_id;
    std::string level;
    std::string result;
    std::string message;
};

struct TaskRunReportParsedRecord {
    std::string raw_file_id;
    labbridge::core::ParsedRecord record;
    std::string parse_status{"parsed"};
    std::vector<TaskRunReportQcResult> qc_results;
};

// 终态报告的文件级失败：read / archive / parse 阶段的具体文件与简短原因。
// archive_raw_file_id 为空表示该输入从未取得有效归档（只能定点补采）。
struct TaskRunReportFailedFile {
    std::string source_path;
    std::string original_name;
    std::string stage;
    std::string message;
    std::string archive_raw_file_id;
};

struct TaskRunReportRequest {
    std::string task_run_id;
    std::string node_code;
    std::string idempotency_key;
    labbridge::core::TaskRunStatus status{
        labbridge::core::TaskRunStatus::Succeeded};
    std::string finished_at;
    int items_total{0};
    int items_success{0};
    int items_failed{0};
    std::string error_summary;
    std::vector<TaskRunReportParsedRecord> parsed_records;
    // has_failed_files 区分“没带字段”和“明确空清单”：
    // 前者沿用旧指纹算法，后者参与指纹且中心落 []。
    bool has_failed_files{false};
    std::vector<TaskRunReportFailedFile> failed_files;
};

struct TaskRunReportResult {
    std::vector<std::string> parsed_record_ids;
    std::vector<std::string> qc_result_ids;
    std::vector<std::string> alert_ids;
    bool replayed{false};
};

std::string make_scheduled_execution_key(
    const std::string& node_code,
    const std::string& task_id,
    const std::string& scheduled_for);
std::string make_manifest_idempotency_key(
    const std::string& node_code,
    const std::string& task_run_id);
std::string make_report_idempotency_key(
    const std::string& node_code,
    const std::string& task_run_id);

// ===== 人工执行（manual / retry）的候选与固定输入 =====

// 创建 retry 时由中心固定的重试目标。
// archive：input 来自既有归档，字段齐全；source：定点补采，只带确切路径，
// 文件内容在实际读取并落计划时才固定。
struct RetryFileInput {
    std::string input_type;
    std::string source_path;
    std::string original_name;
    std::string archive_raw_file_id;
    std::string storage_path;
    long long size_bytes{0};
    std::string file_hash;
    std::string source_mtime;
};

// 配置轮询下发的人工待执行项；retry 带固定 retry_files。
struct PendingExecution {
    std::string task_run_id;
    std::string task_id;
    std::string execution_key;
    std::string trigger_type;
    std::string requested_at;
    std::vector<RetryFileInput> retry_files;
};

class ITaskExecutionClient {
public:
    virtual ~ITaskExecutionClient() = default;
    virtual void request_stop() noexcept {}

    virtual StartTaskRunResult start_task_run(
        const StartTaskRunRequest& request) const = 0;
    virtual RawFileManifestResult report_raw_file_manifest(
        const RawFileManifestRequest& request) const = 0;
    virtual TaskRunReportResult report_task_run(
        const TaskRunReportRequest& request) const = 0;
};

}  // namespace labbridge::agent
