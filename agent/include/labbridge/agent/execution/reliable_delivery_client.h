#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/agent/storage/agent_queue_store.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace labbridge::agent {

class DeliveryAbandoned final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ReliableDeliveryClient final : public ITaskExecutionClient {
public:
    ReliableDeliveryClient(ITaskExecutionClient& client,
                           AgentQueueStore& store,
                           std::chrono::seconds retry_initial,
                           std::chrono::seconds retry_max,
                           std::size_t max_request_body_bytes);

    StartTaskRunResult start_task_run(
        const StartTaskRunRequest& request) const override;
    RawFileManifestResult report_raw_file_manifest(
        const RawFileManifestRequest& request) const override;
    TaskRunReportResult report_task_run(
        const TaskRunReportRequest& request) const override;
    void request_stop() noexcept override;

private:
    template <typename Result, typename Call>
    Result deliver(const std::string& request_type,
                   const std::string& idempotency_key,
                   const std::string& http_body,
                   Call&& call) const;
    // 请求体超过限额：作业转 requires_attention 并终止本次投递，不发送请求。
    [[noreturn]] void abandon_oversized_body(
        const std::string& request_type,
        const std::string& idempotency_key,
        std::size_t body_bytes) const;
    std::chrono::milliseconds retry_delay(
        const std::string& key, int attempt) const;

    ITaskExecutionClient& client_;
    AgentQueueStore& store_;
    std::chrono::seconds retry_initial_;
    std::chrono::seconds retry_max_;
    std::size_t max_request_body_bytes_;
    mutable std::mutex wait_mutex_;
    mutable std::condition_variable wait_condition_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace labbridge::agent
