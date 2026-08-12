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
                           std::chrono::seconds retry_max);

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
                   Call&& call) const;
    std::chrono::milliseconds retry_delay(
        const std::string& key, int attempt) const;

    ITaskExecutionClient& client_;
    AgentQueueStore& store_;
    std::chrono::seconds retry_initial_;
    std::chrono::seconds retry_max_;
    mutable std::mutex wait_mutex_;
    mutable std::condition_variable wait_condition_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace labbridge::agent
