#pragma once

#include "labbridge/agent/execution/reliable_execution_store.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace labbridge::agent {

class AgentQueueStore final : public IReliableExecutionStore {
public:
    AgentQueueStore(std::string database_path,
                    std::string node_code,
                    std::size_t max_pending_jobs,
                    std::size_t processed_fingerprint_capacity = 10000);
    ~AgentQueueStore();
    AgentQueueStore(const AgentQueueStore&) = delete;
    AgentQueueStore& operator=(const AgentQueueStore&) = delete;

    bool begin_job(const labbridge::core::TaskConfig& task,
                   const StartTaskRunRequest& request) override;
    void save_file_plan(const std::string& execution_key,
                        const std::vector<PendingFilePlan>& files) override;
    std::vector<RecoveredJob> recover_jobs() const override;
    RecoveredJob load_job(const std::string& execution_key) const override;
    void accept_start(const std::string& execution_key,
                      const std::string& task_run_id) override;
    void mark_file_archived(const std::string& execution_key,
                            int ordinal) override;
    void save_manifest(const std::string& execution_key,
                       const RawFileManifestRequest& request) override;
    void accept_manifest(const std::string& execution_key,
                         const std::vector<std::string>& raw_file_ids) override;
    void save_report(const std::string& execution_key,
                     const TaskRunReportRequest& request,
                     const std::vector<bool>& parsed_without_errors) override;
    void complete_job(const std::string& execution_key) override;
    void mark_requires_attention(const std::string& execution_key,
                                 const std::string& error_kind,
                                 const std::string& reason) override;
    void record_delivery_failure(
        const std::string& request_type, const std::string& idempotency_key,
        bool retryable, const std::string& error_kind, unsigned int http_status,
        const std::string& message, std::chrono::milliseconds retry_delay);
    void resume_delivery(const std::string& request_type,
                         const std::string& idempotency_key);
    std::chrono::milliseconds delivery_retry_remaining(
        const std::string& request_type, const std::string& idempotency_key) const;
    int delivery_attempt_count(const std::string& request_type,
                               const std::string& idempotency_key) const;
    // 按投递身份（请求类型 + 幂等键）反查所属作业，供投递层转人工时定位作业。
    std::string delivery_execution_key(const std::string& request_type,
                                       const std::string& idempotency_key) const;
    bool has_capacity() const override;
    bool is_file_occupied(const std::string& task_id,
                          const std::string& fingerprint) const override;
    std::size_t pending_job_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ===== queue 维护命令（list/show/retry）用的视图模型 =====

// list 输出的 attention 作业摘要。
struct AttentionJobSummary {
    std::string execution_key;
    std::string task_id;
    std::string error_kind;
    std::string reason;
    std::string updated_at;
};

// show 输出的单个文件条目：源文件与归档的去向。
struct AttentionFileDetail {
    int ordinal{0};
    std::string source_path;
    long long size_bytes{0};
    std::string archive_path;
    std::string archive_state;
};

// show 输出的单条投递：request_type、退避与 HTTP 状态、
// body_bytes 是实际发往控制面的 HTTP body 字节数（容量排查用）。
struct AttentionDeliveryDetail {
    std::string request_type;
    int attempt_count{0};
    std::string next_attempt_at;  // 空串表示没有排定的重试时间
    int last_http_status{0};      // 0 表示还没有 HTTP 状态
    std::string last_error_kind;
    std::string last_error;
    long long body_bytes{0};
};

struct AttentionJobDetail {
    std::string execution_key;
    std::string task_id;
    std::string resume_stage;
    std::string error_kind;
    std::string reason;
    int attempt_count{0};
    std::string started_at;
    std::string updated_at;
    std::vector<AttentionFileDetail> files;
    std::vector<AttentionDeliveryDetail> deliveries;
};

enum class QueueRetryStatus {
    Resumed,       // 已允许恢复，下次 Agent 启动接着跑
    NotAttention,  // 状态已不是 requires_attention，没有做任何修改
};

struct QueueRetryResult {
    QueueRetryStatus status{QueueRetryStatus::NotAttention};
    // Resumed 时是恢复到的执行阶段；NotAttention 时是作业当前阶段。
    std::string stage;
};

// 维护命令打开已有队列的入口：不建目录、不建库、不初始化 schema，
// 路径错误、空库、schema 不支持或节点身份不符都直接报错。
// read_only 为 true 用只读连接（list/show），false 用不带 CREATE 的
// 读写连接（retry），不复用运行时构造器的建库路径。
class AgentQueueMaintenance final {
public:
    AgentQueueMaintenance(const std::string& database_path,
                          const std::string& node_code,
                          bool read_only);
    ~AgentQueueMaintenance();
    AgentQueueMaintenance(const AgentQueueMaintenance&) = delete;
    AgentQueueMaintenance& operator=(const AgentQueueMaintenance&) = delete;

    std::vector<AttentionJobSummary> list_attention_jobs() const;
    AttentionJobDetail load_attention_job(
        const std::string& execution_key) const;
    QueueRetryResult retry_attention_job(const std::string& execution_key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 队列路径旁的 flock 独占锁：运行中的 Agent 启动时拿住直到进程退出，
// queue retry 非阻塞抢锁，抢不到说明 Agent 还在运行，先停服务。
// 锁由内核在持有进程退出时释放，不用 PID 文件推断进程死活；
// list/show 是只读查询，不抢这把锁。
class AgentQueueLock final {
public:
    explicit AgentQueueLock(const std::string& database_path);
    ~AgentQueueLock();
    AgentQueueLock(const AgentQueueLock&) = delete;
    AgentQueueLock& operator=(const AgentQueueLock&) = delete;

private:
    int file_descriptor_{-1};
};

}  // namespace labbridge::agent
