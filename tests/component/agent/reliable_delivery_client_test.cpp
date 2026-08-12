#include "labbridge/agent/execution/execution_request_codec.h"
#include "labbridge/agent/execution/reliable_delivery_client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
using namespace std::chrono_literals;

labbridge::core::TaskConfig task() {
    labbridge::core::TaskConfig value;
    value.id = "24-03";
    value.node_code = "node-024";
    value.name = "offline retry";
    value.task_type = "local_file_import";
    value.schedule_expr = "* * * * *";
    value.parser_type = "csv_observation";
    value.data_source_id = "source-024";
    value.data_source.id = "source-024";
    value.data_source.node_code = "node-024";
    value.data_source.config_json = R"({"root_path":"/tmp","extension":".csv"})";
    return value;
}

labbridge::agent::StartTaskRunRequest request() {
    return {"node-024", "24-03", "execution-024-03",
            "2026-08-12T10:00:00Z", "2026-08-12T10:00:01Z", "scheduled"};
}

class FailingClient final : public labbridge::agent::ITaskExecutionClient {
public:
    explicit FailingClient(labbridge::agent::TaskExecutionClientError error)
        : error_(std::move(error)) {}
    labbridge::agent::StartTaskRunResult start_task_run(
        const labbridge::agent::StartTaskRunRequest&) const override {
        ++calls;
        throw error_;
    }
    labbridge::agent::RawFileManifestResult report_raw_file_manifest(
        const labbridge::agent::RawFileManifestRequest&) const override {
        return {};
    }
    labbridge::agent::TaskRunReportResult report_task_run(
        const labbridge::agent::TaskRunReportRequest&) const override {
        return {};
    }
    mutable std::atomic<int> calls{0};
private:
    labbridge::agent::TaskExecutionClientError error_;
};

TEST(ReliableDeliveryClientTest, PermanentConflictMovesJobToAttention) {
    const auto path = std::filesystem::temp_directory_path() /
                      "labbridge-phase024-03-attention.db";
    std::filesystem::remove(path);
    labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
    store.begin_job(task(), request());
    FailingClient client{{labbridge::agent::TaskExecutionErrorKind::HttpStatus,
                          "idempotency conflict", 409}};
    labbridge::agent::ReliableDeliveryClient reliable{client, store, 1s, 2s};

    EXPECT_THROW(reliable.start_task_run(request()),
                 labbridge::agent::DeliveryAbandoned);
    EXPECT_EQ(store.delivery_attempt_count("start", "execution-024-03"), 1);
    EXPECT_TRUE(store.recover_jobs().empty());
    EXPECT_EQ(store.pending_job_count(), 1U);
    std::cout << "delivery_outcome=requires_attention http_status=409 "
                 "pending_jobs=1 auto_retry=false\n";
    std::filesystem::remove(path);
}

TEST(ReliableDeliveryClientTest, StopInterruptsRetryAndKeepsPendingJob) {
    const auto path = std::filesystem::temp_directory_path() /
                      "labbridge-phase024-03-stop.db";
    std::filesystem::remove(path);
    labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
    store.begin_job(task(), request());
    FailingClient client{{labbridge::agent::TaskExecutionErrorKind::Network,
                          "server offline"}};
    labbridge::agent::ReliableDeliveryClient reliable{client, store, 30s, 30s};
    std::thread worker{[&] {
        EXPECT_THROW(reliable.start_task_run(request()),
                     labbridge::agent::DeliveryAbandoned);
    }};
    while (client.calls.load() == 0) {
        std::this_thread::yield();
    }
    reliable.request_stop();
    worker.join();

    EXPECT_EQ(store.pending_job_count(), 1U);
    EXPECT_EQ(store.delivery_attempt_count("start", "execution-024-03"), 1);
    std::cout << "retry_wait=interrupted pending_jobs=1 "
                 "synthetic_failed_report=0\n";
    std::filesystem::remove(path);
}
}  // namespace
