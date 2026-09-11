#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/agent/execution/execution_request_codec.h"
#include "labbridge/agent/execution/reliable_delivery_client.h"
#include "labbridge/agent/execution/task_executor.h"
#include "labbridge/agent/storage/agent_queue_store.h"
#include "labbridge/core/utc_time.h"
#include "support/agent/mock_http_server.h"

#include <gtest/gtest.h>

#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace http = boost::beast::http;
using labbridge::test::support::MockHttpServer;
using labbridge::test::support::local_server_url;
using namespace std::chrono_literals;

// 与服务端凭据格式约束一致的 64 位十六进制密钥。
const std::string kStaleToken(64, 'a');
const std::string kValidToken(64, 'b');
const std::string kNodeCode = "phase027-recovery-node";

class AuthRecoveryTree final {
public:
    AuthRecoveryTree() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch().count()) +
            "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        root_ = std::filesystem::temp_directory_path() /
                ("labbridge-auth-recovery-" + suffix);
        inbox_ = root_ / "inbox";
        work_ = root_ / "work";
        std::filesystem::create_directories(inbox_);
    }

    ~AuthRecoveryTree() {
        std::error_code ignored;
        for (auto iterator = inbox_files_.rbegin();
             iterator != inbox_files_.rend(); ++iterator) {
            std::filesystem::remove(*iterator, ignored);
        }
        for (auto iterator = archive_files_.rbegin();
             iterator != archive_files_.rend(); ++iterator) {
            std::filesystem::remove(*iterator, ignored);
        }
        std::filesystem::remove(work_, ignored);
        std::filesystem::remove(inbox_, ignored);
        std::filesystem::remove(queue_database(), ignored);
        std::filesystem::remove(queue_database().string() + "-wal", ignored);
        std::filesystem::remove(queue_database().string() + "-shm", ignored);
        std::filesystem::remove(root_, ignored);
    }

    void write_csv(const std::string& name, const std::string& content) {
        const auto path = inbox_ / name;
        std::ofstream output{path};
        output << content;
        if (!output.good()) {
            throw std::runtime_error("failed to write CSV fixture");
        }
        inbox_files_.push_back(path);
    }

    // 归档文件由执行器在 work 下自建，析构前登记进来才能把目录删干净。
    void track_archive(const std::string& storage_path) {
        archive_files_.emplace_back(storage_path);
    }

    const std::filesystem::path& inbox() const { return inbox_; }
    const std::filesystem::path& work() const { return work_; }
    std::filesystem::path queue_database() const { return root_ / "queue.db"; }

private:
    std::filesystem::path root_;
    std::filesystem::path inbox_;
    std::filesystem::path work_;
    std::vector<std::filesystem::path> inbox_files_;
    std::vector<std::filesystem::path> archive_files_;
};

// 用作用域模拟对象重建；真正的线程退出由 AgentApplication 测试覆盖。
class AgentProcess final {
public:
    AgentProcess(const AuthRecoveryTree& tree,
                 const std::string& base_url,
                 const std::string& token)
        : client_{base_url, std::chrono::milliseconds{2000}, kNodeCode, token},
          store_{tree.queue_database().string(), kNodeCode, 10, 10},
          delivery_{client_, store_, std::chrono::seconds{1},
                    std::chrono::seconds{2}},
          executor_{delivery_, store_, tree.work(), {tree.inbox()},
                    fixed_now} {}

    labbridge::agent::TaskExecutor& executor() { return executor_; }
    labbridge::agent::AgentQueueStore& store() { return store_; }

private:
    static std::chrono::system_clock::time_point fixed_now() {
        // 固定时钟让 execution_key 稳定，多次恢复针对同一条作业。
        return std::chrono::system_clock::time_point{} + 1786176000s;
    }

    labbridge::agent::ControlPlaneClient client_;
    labbridge::agent::AgentQueueStore store_;
    labbridge::agent::ReliableDeliveryClient delivery_;
    labbridge::agent::TaskExecutor executor_;
};

labbridge::core::TaskConfig recovery_task(const std::filesystem::path& inbox) {
    labbridge::core::TaskConfig task;
    task.id = "30";
    task.node_code = kNodeCode;
    task.data_source_id = "10";
    task.name = "auth recovery csv";
    task.task_type = "local_file_import";
    task.schedule_expr = "* * * * *";
    task.parser_type = "csv_observation";
    task.enabled = true;
    task.data_source.id = "10";
    task.data_source.node_code = kNodeCode;
    task.data_source.type = labbridge::core::SourceType::LocalDirectory;
    task.data_source.name = "recovery inbox";
    task.data_source.config_json =
        "{\"root_path\":\"" + inbox.string() +
        "\",\"extension\":\".csv\"}";
    task.qc_rules = {
        {"21", "required_fields", "required fields", "{}"},
        {"22", "basic_timestamp_format", "timestamp", "{}"},
    };
    return task;
}

labbridge::agent::ScheduledTaskExecution scheduled(
    labbridge::core::TaskConfig task) {
    return {
        std::move(task),
        std::chrono::system_clock::time_point{} + 1786176000s,
    };
}

class AuthRecoveryExecutionTest : public testing::TestWithParam<int> {};

TEST_P(AuthRecoveryExecutionTest,
     SurvivesAuthRejectionAtEveryStageAndRecoversAfterCredentialFix) {
    AuthRecoveryTree tree;
    tree.write_csv(
        "recovery.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-09-10 08:00:00,42\n");
    const auto slot = scheduled(recovery_task(tree.inbox()));

    const std::string rejection_body = GetParam() == 401
        ? R"({"ok":false,"error":{"code":"unauthenticated","message":"token rejected"}})"
        : R"({"ok":false,"error":{"code":"forbidden","message":"node rejected"}})";
    const auto expect_auth_rejection = [&](auto operation) {
        try {
            operation();
            FAIL() << "expected authentication rejection";
        } catch (const labbridge::agent::TaskExecutionClientError& error) {
            EXPECT_EQ(error.http_status(), GetParam());
            EXPECT_TRUE(error.is_auth_rejection());
        }
    };

    // 按请求顺序回放：start 拒/成、manifest 拒/成、report 拒/成。
    MockHttpServer server{{
        {static_cast<http::status>(GetParam()), rejection_body},
        {http::status::created,
         R"({"ok":true,"data":{"task_run_id":"901","replayed":false}})"},
        {static_cast<http::status>(GetParam()), rejection_body},
        {http::status::created,
         R"({"ok":true,"data":{"raw_file_ids":["951"],"replayed":false}})"},
        {static_cast<http::status>(GetParam()), rejection_body},
        {http::status::ok,
         R"({"ok":true,"data":{"parsed_record_ids":["961"],)"
         R"("qc_result_ids":["971","972"],"alert_ids":[],"replayed":false}})"},
    }};
    const auto base_url = local_server_url(server.port());

    // 进程 1：密钥配错，start 直接被拒绝，作业留在 start_pending。
    {
        AgentProcess process{tree, base_url, kStaleToken};
        expect_auth_rejection([&] { process.executor().execute(slot); });
        EXPECT_EQ(process.store().pending_job_count(), 1U);
        const auto jobs = process.store().recover_jobs();
        ASSERT_EQ(jobs.size(), 1U);
        EXPECT_EQ(jobs.front().stage, "start_pending");
        // 断点上的请求就是原调度请求，恢复后按同一幂等键重投。
        EXPECT_EQ(
            jobs.front().start_request.execution_key,
            labbridge::agent::make_scheduled_execution_key(
                kNodeCode, "30",
                labbridge::core::format_utc_timestamp(slot.scheduled_for)));
    }

    // 进程 2：修正密钥重启。start 重投成功，本次 manifest 被拒，
    // 作业停在 manifest_pending，归档证据已经落盘。
    {
        AgentProcess process{tree, base_url, kValidToken};
        expect_auth_rejection([&] { process.executor().recover_pending_jobs(); });
        const auto jobs = process.store().recover_jobs();
        ASSERT_EQ(jobs.size(), 1U);
        EXPECT_EQ(jobs.front().stage, "manifest_pending");
        ASSERT_EQ(jobs.front().manifest_request.files.size(), 1U);
        tree.track_archive(
            jobs.front().manifest_request.files.front().storage_path);
    }

    // 进程 3：manifest 重投成功，report 被拒，作业停在 report_pending。
    {
        AgentProcess process{tree, base_url, kValidToken};
        expect_auth_rejection([&] { process.executor().recover_pending_jobs(); });
        const auto jobs = process.store().recover_jobs();
        ASSERT_EQ(jobs.size(), 1U);
        EXPECT_EQ(jobs.front().stage, "report_pending");
        EXPECT_EQ(jobs.front().report_request.status,
                  labbridge::core::TaskRunStatus::Succeeded);
    }

    // 进程 4：report 重投成功，作业完成，队列清空。
    {
        AgentProcess process{tree, base_url, kValidToken};
        process.executor().recover_pending_jobs();
        EXPECT_EQ(process.store().pending_job_count(), 0U);
        EXPECT_TRUE(process.store().recover_jobs().empty());
    }

    ASSERT_NO_THROW(server.join());

    // 每次恢复都重发原请求，已收下的 manifest 回执也留在队列里。
    for (std::size_t index = 0; index < server.requests().size(); index += 2) {
        EXPECT_EQ(server.requests()[index].body, server.requests()[index + 1].body);
    }
    ASSERT_EQ(server.requests().size(), 6U);
    const std::vector<std::string> targets = {
        "/api/v1/task-runs/start",
        "/api/v1/task-runs/start",
        "/api/v1/raw-files/manifest",
        "/api/v1/raw-files/manifest",
        "/api/v1/task-runs/report",
        "/api/v1/task-runs/report",
    };
    for (std::size_t index = 0; index < server.requests().size(); ++index) {
        EXPECT_EQ(server.requests()[index].target, targets[index]);
        EXPECT_EQ(server.requests()[index].node_code, kNodeCode)
            << targets[index];
    }
    EXPECT_EQ(server.requests()[0].authorization, "Bearer " + kStaleToken);
    for (std::size_t index = 1; index < server.requests().size(); ++index) {
        EXPECT_EQ(server.requests()[index].authorization,
                  "Bearer " + kValidToken)
            << targets[index];
    }

    std::cout << "auth_recovery stages=start/manifest/report "
                 "http_requests=6 pending_jobs=0 new_token_reused=true"
              << std::endl;
}

INSTANTIATE_TEST_SUITE_P(
    UnauthorizedAndForbidden, AuthRecoveryExecutionTest, testing::Values(401, 403));

}  // namespace
