#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/agent/execution/execution_request_codec.h"
#include "labbridge/agent/execution/reliable_delivery_client.h"
#include "support/agent/mock_http_server.h"

#include <gtest/gtest.h>

#include <boost/beast/http.hpp>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
using namespace std::chrono_literals;

namespace http = boost::beast::http;
using labbridge::test::support::MockHttpServer;
using labbridge::test::support::local_server_url;

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
    value.data_source.type =
        labbridge::core::SourceType::LocalDirectory;
    value.data_source.name = "offline retry source";
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
        ++calls;
        throw error_;
    }
    labbridge::agent::TaskRunReportResult report_task_run(
        const labbridge::agent::TaskRunReportRequest&) const override {
        return {};
    }
    mutable std::atomic<int> calls{0};
private:
    labbridge::agent::TaskExecutionClientError error_;
};

// 队列里铺一条推进到 manifest_pending 的作业，返回已持久化的 manifest 请求。
labbridge::agent::RawFileManifestRequest seed_manifest_job(
    labbridge::agent::AgentQueueStore& store) {
    store.begin_job(task(), request(), {});
    store.accept_start(request().execution_key, "run-1");
    labbridge::agent::RawFileManifestRequest manifest{
        "run-1",
        "node-024",
        "manifest-key-030",
        {{"sample.csv", std::string(64, 'a'), "/archive/sample.csv", 42,
          "2026-08-12T00:00:00Z", "archived_local"}},
    };
    store.save_manifest(request().execution_key, manifest);
    return manifest;
}

// 直连 SQLite 读作业的阶段与错误种类，验证 attention 转换在库里保存了什么。
struct JobStageRow {
    std::string stage;
    std::string retry_stage;
    std::string last_error_kind;
};

JobStageRow read_job_row(const std::filesystem::path& database,
                         const std::string& execution_key) {
    sqlite3* handle = nullptr;
    EXPECT_EQ(sqlite3_open_v2(database.string().c_str(), &handle,
                              SQLITE_OPEN_READONLY, nullptr),
              SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
                  handle,
                  "SELECT stage, COALESCE(retry_stage, ''), "
                  "COALESCE(last_error_kind, '') FROM pending_jobs "
                  "WHERE execution_key = ?",
                  -1, &statement, nullptr),
              SQLITE_OK);
    sqlite3_bind_text(statement, 1, execution_key.c_str(), -1,
                      SQLITE_TRANSIENT);
    EXPECT_EQ(sqlite3_step(statement), SQLITE_ROW);
    JobStageRow row{
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
    };
    sqlite3_finalize(statement);
    sqlite3_close_v2(handle);
    return row;
}

TEST(ReliableDeliveryClientTest, PermanentConflictMovesJobToAttention) {
    const auto path = std::filesystem::temp_directory_path() /
                      "labbridge-phase024-03-attention.db";
    std::filesystem::remove(path);
    labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
    store.begin_job(task(), request(), {});
    FailingClient client{{labbridge::agent::TaskExecutionErrorKind::HttpStatus,
                          "idempotency conflict", 409}};
    labbridge::agent::ReliableDeliveryClient reliable{
        client, store, 1s, 2s, labbridge::agent::kDefaultMaxRequestBodyBytes};

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
    store.begin_job(task(), request(), {});
    FailingClient client{{labbridge::agent::TaskExecutionErrorKind::Network,
                          "server offline"}};
    labbridge::agent::ReliableDeliveryClient reliable{
        client, store, 30s, 30s, labbridge::agent::kDefaultMaxRequestBodyBytes};
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

// 服务端的业务拒绝走统一错误包络，客户端会被归成 ServerError；
// 这里用真实 HTTP 回放确认 400/409/413 首次失败即转人工处理。
class PermanentEnvelopeDeliveryTest
    : public testing::TestWithParam<int> {};

TEST_P(PermanentEnvelopeDeliveryTest,
       AbandonsStructuredEnvelopeRejectionOnFirstAttempt) {
    const auto path = std::filesystem::temp_directory_path() /
                      "labbridge-permanent-envelope.db";
    std::filesystem::remove(path);
    MockHttpServer server{{
        {static_cast<http::status>(GetParam()),
         R"({"ok":false,"error":{"code":"rejected","message":"permanent rejection"}})"},
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()), std::chrono::milliseconds{2000},
        "node-024", std::string(64, 'a')};
    labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
    store.begin_job(task(), request(), {});
    labbridge::agent::ReliableDeliveryClient reliable{
        client, store, 1s, 2s, labbridge::agent::kDefaultMaxRequestBodyBytes};

    EXPECT_THROW(reliable.start_task_run(request()),
                 labbridge::agent::DeliveryAbandoned);
    ASSERT_NO_THROW(server.join());

    EXPECT_EQ(store.delivery_attempt_count("start", "execution-024-03"), 1);
    EXPECT_EQ(server.requests().size(), 1U);
    // 转人工的作业不再参与自动恢复，但请求和幂等键都还留在队列里。
    EXPECT_TRUE(store.recover_jobs().empty());
    EXPECT_EQ(store.pending_job_count(), 1U);
    std::cout << "permanent_envelope http_status=" << GetParam()
              << " attempts=1 outcome=requires_attention" << std::endl;
    std::filesystem::remove(path);
}

INSTANTIATE_TEST_SUITE_P(
    BadRequestConflictAndTooLarge, PermanentEnvelopeDeliveryTest,
    testing::Values(400, 409, 413));

TEST(ReliableDeliveryClientTest, RetryableEnvelopeStatusKeepsJobPending) {
    const auto path = std::filesystem::temp_directory_path() /
                      "labbridge-retryable-envelope.db";
    std::filesystem::remove(path);
    MockHttpServer server{{
        {http::status::too_many_requests,
         R"({"ok":false,"error":{"code":"rate_limited","message":"slow down"}})"},
    }};
    labbridge::agent::ControlPlaneClient client{
        local_server_url(server.port()), std::chrono::milliseconds{2000},
        "node-024", std::string(64, 'a')};
    labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
    store.begin_job(task(), request(), {});
    labbridge::agent::ReliableDeliveryClient reliable{
        client, store, 30s, 30s, labbridge::agent::kDefaultMaxRequestBodyBytes};
    std::thread worker{[&] {
        EXPECT_THROW(reliable.start_task_run(request()),
                     labbridge::agent::DeliveryAbandoned);
    }};
    // 等失败已落库再停止：SQLite 层自带锁，可以放心轮询。不要直接读
    // mock server 的请求列表——serve 线程还在往里写，无同步访问是数据竞争。
    while (store.delivery_attempt_count("start", "execution-024-03") == 0) {
        std::this_thread::yield();
    }
    // 429 属于瞬时状态：失败后进入退避等待，被停止打断而不是转人工。
    reliable.request_stop();
    worker.join();
    ASSERT_NO_THROW(server.join());

    EXPECT_EQ(store.delivery_attempt_count("start", "execution-024-03"), 1);
    EXPECT_EQ(store.pending_job_count(), 1U);
    EXPECT_EQ(store.recover_jobs().front().stage, "start_pending");
    std::filesystem::remove(path);
}

TEST(ReliableDeliveryClientTest, StopInterruptsStartupRecoveryBackoff) {
    const auto path = std::filesystem::temp_directory_path() /
                      "labbridge-startup-backoff.db";
    std::filesystem::remove(path);
    FailingClient client{{labbridge::agent::TaskExecutionErrorKind::Network,
                          "server offline"}};

    // 第一轮：网络失败把作业打入 retry_wait，退避 30 秒。
    {
        labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
        store.begin_job(task(), request(), {});
        labbridge::agent::ReliableDeliveryClient reliable{
            client, store, 30s, 30s,
            labbridge::agent::kDefaultMaxRequestBodyBytes};
        std::thread worker{[&] {
            EXPECT_THROW(reliable.start_task_run(request()),
                         labbridge::agent::DeliveryAbandoned);
        }};
        while (store.delivery_attempt_count(
                   "start", "execution-024-03") == 0) {
            std::this_thread::yield();
        }
        reliable.request_stop();
        worker.join();
    }

    // 重启后要先补齐剩余退避时间；这段等待同样要能被停止请求打断，
    // 否则停机时进程会在这个等待上挂住半分钟。
    {
        labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
        labbridge::agent::ReliableDeliveryClient reliable{
            client, store, 30s, 30s,
            labbridge::agent::kDefaultMaxRequestBodyBytes};
        const auto started = std::chrono::steady_clock::now();
        std::thread worker{[&] {
            EXPECT_THROW(reliable.start_task_run(request()),
                         labbridge::agent::DeliveryAbandoned);
        }};
        std::this_thread::sleep_for(200ms);
        reliable.request_stop();
        worker.join();
        EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);

        // 等待被打断：没有发起新请求，也没有 resume，作业原样留在队列里。
        EXPECT_EQ(client.calls.load(), 1);
        EXPECT_EQ(store.delivery_attempt_count("start", "execution-024-03"), 1);
        EXPECT_EQ(store.pending_job_count(), 1U);
        std::cout << "startup_backoff interrupted=true waited_ms<5000 "
                     "pending_jobs=1" << std::endl;
    }
    std::filesystem::remove(path);
}

TEST(ReliableDeliveryClientTest, PayloadLimitAllowsExactSizeAndRejectsOneByteMore) {
    // 等于上限：正好放行，服务端收到的 body 与大小检查用的是同一份编码。
    {
        const auto path = std::filesystem::temp_directory_path() /
                          "labbridge-payload-limit-exact.db";
        std::filesystem::remove(path);
        MockHttpServer server{{
            {http::status::ok,
             R"({"ok":true,"data":{"raw_file_ids":["raw-1"],"replayed":false}})"},
        }};
        labbridge::agent::ControlPlaneClient client{
            local_server_url(server.port()), std::chrono::milliseconds{2000},
            "node-024", std::string(64, 'a')};
        labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
        const auto manifest = seed_manifest_job(store);
        const auto body =
            labbridge::agent::encode_raw_file_manifest_http_body(manifest);
        labbridge::agent::ReliableDeliveryClient exact{
            client, store, 1s, 2s, body.size()};

        const auto result = exact.report_raw_file_manifest(manifest);
        ASSERT_NO_THROW(server.join());

        EXPECT_EQ(result.raw_file_ids, (std::vector<std::string>{"raw-1"}));
        ASSERT_EQ(server.requests().size(), 1U);
        EXPECT_EQ(server.requests().front().body, body);
        std::filesystem::remove(path);
    }

    // 超过一个字节：不发请求，作业转 attention 并保存原执行阶段。
    {
        const auto path = std::filesystem::temp_directory_path() /
                          "labbridge-payload-limit-over.db";
        std::filesystem::remove(path);
        MockHttpServer server{{
            {http::status::ok,
             R"({"ok":true,"data":{"raw_file_ids":["raw-1"],"replayed":false}})"},
        }};
        labbridge::agent::ControlPlaneClient client{
            local_server_url(server.port()), std::chrono::milliseconds{2000},
            "node-024", std::string(64, 'a')};
        labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
        const auto manifest = seed_manifest_job(store);
        const auto body =
            labbridge::agent::encode_raw_file_manifest_http_body(manifest);
        labbridge::agent::ReliableDeliveryClient over{
            client, store, 1s, 2s, body.size() - 1};

        EXPECT_THROW(over.report_raw_file_manifest(manifest),
                     labbridge::agent::DeliveryAbandoned);
        // 超限请求从未发出：mock server 没收到连接，不显式 join，
        // 交给析构唤醒 accept 线程。
        ASSERT_EQ(server.requests().size(), 0U);

        const auto row = read_job_row(path, request().execution_key);
        EXPECT_EQ(row.stage, "requires_attention");
        EXPECT_EQ(row.retry_stage, "manifest_pending");
        EXPECT_EQ(row.last_error_kind, "payload_too_large");
        // 大小检查在投递尝试计数之前，一次尝试都没发生。
        EXPECT_EQ(store.delivery_attempt_count("manifest", "manifest-key-030"), 0);
        std::filesystem::remove(path);
    }
}

TEST(ReliableDeliveryClientTest,
     RetryWaitRecoveryOversizedKeepsOriginalRetryStage) {
    const auto path = std::filesystem::temp_directory_path() /
                      "labbridge-payload-retrywait.db";
    std::filesystem::remove(path);
    FailingClient network_client{
        {labbridge::agent::TaskExecutionErrorKind::Network, "server offline"}};
    labbridge::agent::RawFileManifestRequest manifest;
    {
        labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
        manifest = seed_manifest_job(store);

        // 第一次投递网络失败 -> retry_wait，退避 30 秒，用停止请求打断等待。
        labbridge::agent::ReliableDeliveryClient healthy{
            network_client, store, 30s, 30s,
            labbridge::agent::kDefaultMaxRequestBodyBytes};
        std::thread worker{[&] {
            EXPECT_THROW(healthy.report_raw_file_manifest(manifest),
                         labbridge::agent::DeliveryAbandoned);
        }};
        while (store.delivery_attempt_count("manifest", "manifest-key-030") == 0) {
            std::this_thread::yield();
        }
        healthy.request_stop();
        worker.join();

        const auto row = read_job_row(path, request().execution_key);
        ASSERT_EQ(row.stage, "retry_wait");
        ASSERT_EQ(row.retry_stage, "manifest_pending");
    }

    // 重启恢复时限额已小于请求体：检查必须先于退避等待触发，
    // 否则要白等 30 秒才知道超限。
    {
        labbridge::agent::AgentQueueStore store{path.string(), "node-024", 10};
        labbridge::agent::ReliableDeliveryClient tiny{
            network_client, store, 30s, 30s,
            labbridge::agent::encode_raw_file_manifest_http_body(manifest).size() - 1};
        const auto started = std::chrono::steady_clock::now();
        EXPECT_THROW(tiny.report_raw_file_manifest(manifest),
                     labbridge::agent::DeliveryAbandoned);
        EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);

        const auto row = read_job_row(path, request().execution_key);
        EXPECT_EQ(row.stage, "requires_attention");
        // 原执行阶段原样保留，人工恢复后从 manifest_pending 继续。
        EXPECT_EQ(row.retry_stage, "manifest_pending");
        EXPECT_EQ(row.last_error_kind, "payload_too_large");
    }
    std::filesystem::remove(path);
}
}  // namespace
