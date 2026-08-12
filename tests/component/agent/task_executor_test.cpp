#include "labbridge/agent/execution/task_executor.h"
#include "labbridge/agent/storage/agent_queue_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

class TemporaryExecutionTree final {
public:
    TemporaryExecutionTree() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()) +
            "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        root_ = std::filesystem::temp_directory_path() /
                ("labbridge-task-executor-" + suffix);
        inbox_ = root_ / "inbox";
        work_ = root_ / "work";
        std::filesystem::create_directories(inbox_);
    }

    ~TemporaryExecutionTree() {
        std::error_code ignored;
        for (auto iterator = files_.rbegin(); iterator != files_.rend();
             ++iterator) {
            std::filesystem::remove(*iterator, ignored);
        }
        for (auto iterator = archive_files_.rbegin();
             iterator != archive_files_.rend(); ++iterator) {
            std::filesystem::remove(*iterator, ignored);
        }
        for (auto iterator = archive_directories_.rbegin();
             iterator != archive_directories_.rend(); ++iterator) {
            std::filesystem::remove(*iterator, ignored);
        }
        std::filesystem::remove(work_, ignored);
        std::filesystem::remove(inbox_, ignored);
        std::filesystem::remove(root_, ignored);
    }

    std::filesystem::path write_csv(const std::string& name,
                                    const std::string& content) {
        const auto path = inbox_ / name;
        std::ofstream output{path};
        output << content;
        if (!output.good()) {
            throw std::runtime_error("failed to write CSV fixture");
        }
        files_.push_back(path);
        return path;
    }

    void block_work_directory_with_file() {
        std::ofstream output{work_};
        output << "not a directory";
        if (!output.good()) {
            throw std::runtime_error("failed to create blocking work file");
        }
        files_.push_back(work_);
    }

    void track_archive(const std::string& storage_path) {
        const std::filesystem::path file{storage_path};
        archive_files_.push_back(file);
        auto directory = file.parent_path();
        while (directory != work_ && directory != directory.root_path()) {
            archive_directories_.push_back(directory);
            directory = directory.parent_path();
        }
        std::sort(
            archive_directories_.begin(), archive_directories_.end(),
            [](const auto& left, const auto& right) {
                return left.native().size() > right.native().size();
            });
        archive_directories_.erase(
            std::unique(
                archive_directories_.begin(), archive_directories_.end()),
            archive_directories_.end());
    }

    const std::filesystem::path& inbox() const {
        return inbox_;
    }

    const std::filesystem::path& work() const {
        return work_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path inbox_;
    std::filesystem::path work_;
    std::vector<std::filesystem::path> files_;
    std::vector<std::filesystem::path> archive_files_;
    std::vector<std::filesystem::path> archive_directories_;
};

class FakeExecutionClient final
    : public labbridge::agent::ITaskExecutionClient {
public:
    labbridge::agent::StartTaskRunResult start_task_run(
        const labbridge::agent::StartTaskRunRequest& request) const override {
        events.push_back("start");
        starts.push_back(request);
        if (on_start) {
            on_start();
        }
        return {"run-" + std::to_string(++run_sequence), false};
    }

    labbridge::agent::RawFileManifestResult report_raw_file_manifest(
        const labbridge::agent::RawFileManifestRequest& request) const override {
        events.push_back("manifest");
        manifests.push_back(request);
        labbridge::agent::RawFileManifestResult result;
        if (!return_wrong_manifest_count) {
            for (std::size_t index = 0; index < request.files.size(); ++index) {
                result.raw_file_ids.push_back(
                    "raw-" + std::to_string(index + 1));
            }
        }
        return result;
    }

    labbridge::agent::TaskRunReportResult report_task_run(
        const labbridge::agent::TaskRunReportRequest& request) const override {
        events.push_back("report");
        reports.push_back(request);
        if (fail_next_report) {
            fail_next_report = false;
            throw std::runtime_error("simulated report transport failure");
        }
        return {};
    }

    mutable std::vector<std::string> events;
    mutable std::vector<labbridge::agent::StartTaskRunRequest> starts;
    mutable std::vector<labbridge::agent::RawFileManifestRequest> manifests;
    mutable std::vector<labbridge::agent::TaskRunReportRequest> reports;
    mutable int run_sequence{0};
    mutable bool fail_next_report{false};
    std::function<void()> on_start;
    bool return_wrong_manifest_count{false};
};

labbridge::core::TaskConfig executable_task(
    const std::filesystem::path& inbox) {
    labbridge::core::TaskConfig task;
    task.id = "30";
    task.node_code = "phase022-node";
    task.data_source_id = "10";
    task.name = "local CSV review";
    task.task_type = "local_file_import";
    task.schedule_expr = "* * * * *";
    task.parser_type = "csv_observation";
    task.enabled = true;
    task.data_source.id = "10";
    task.data_source.node_code = task.node_code;
    task.data_source.type = labbridge::core::SourceType::LocalDirectory;
    task.data_source.name = "review inbox";
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

auto fixed_now() {
    return [] {
        return std::chrono::system_clock::time_point{} + 1786176001s;
    };
}

TEST(TaskExecutorTest, StopBeforeStartDoesNotCreateTaskRun) {
    TemporaryExecutionTree tree;
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.request_stop();
    executor.execute(scheduled(executable_task(tree.inbox())));

    EXPECT_TRUE(client.events.empty());
    std::cout << "idle_stop start_requests=0 terminal_reports=0" << std::endl;
}

TEST(TaskExecutorTest, StopAfterStartDrainsOneFailedTerminalReport) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "must-not-be-collected.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n");
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};
    client.on_start = [&executor] { executor.request_stop(); };

    executor.execute(scheduled(executable_task(tree.inbox())));

    EXPECT_EQ(
        client.events,
        (std::vector<std::string>{"start", "report"}));
    ASSERT_EQ(client.reports.size(), 1U);
    EXPECT_EQ(
        client.reports.front().status,
        labbridge::core::TaskRunStatus::Failed);
    EXPECT_NE(
        client.reports.front().error_summary.find("collection skipped"),
        std::string::npos);
    EXPECT_TRUE(client.manifests.empty());
    std::cout << "active_stop flow=start->drain_report status=failed "
              << "manifest_requests=0 terminal_reports=1" << std::endl;
}

TEST(TaskExecutorTest, ArchivesParsesQcAndReportsOneTerminalResult) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "observations.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n"
        ",DV002,invalid,43\n");
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(executable_task(tree.inbox())));

    EXPECT_EQ(
        client.events,
        (std::vector<std::string>{"start", "manifest", "report"}));
    ASSERT_EQ(client.manifests.size(), 1U);
    ASSERT_EQ(client.manifests.front().files.size(), 1U);
    const auto& manifest = client.manifests.front().files.front();
    tree.track_archive(manifest.storage_path);
    EXPECT_TRUE(std::filesystem::is_regular_file(manifest.storage_path));
    EXPECT_EQ(manifest.ingest_status, "archived_local");
    EXPECT_EQ(manifest.file_hash.size(), 64U);
    EXPECT_EQ(
        std::filesystem::file_size(manifest.storage_path),
        static_cast<std::uintmax_t>(manifest.size_bytes));

    ASSERT_EQ(client.reports.size(), 1U);
    const auto& report = client.reports.front();
    EXPECT_EQ(report.status, labbridge::core::TaskRunStatus::Succeeded);
    EXPECT_EQ(report.items_total, 2);
    EXPECT_EQ(report.items_success, 2);
    EXPECT_EQ(report.items_failed, 0);
    EXPECT_TRUE(report.error_summary.empty());
    ASSERT_EQ(report.parsed_records.size(), 2U);
    ASSERT_EQ(report.parsed_records[0].qc_results.size(), 2U);
    EXPECT_EQ(report.parsed_records[0].qc_results[0].qc_rule_id, "21");
    EXPECT_EQ(report.parsed_records[0].qc_results[1].qc_rule_id, "22");
    EXPECT_EQ(report.parsed_records[1].qc_results[0].result, "failed");
    EXPECT_EQ(report.parsed_records[1].qc_results[1].result, "failed");

    std::cout << "business_flow=start->archive->manifest->parse->qc->report "
              << "run_status=succeeded records=2 qc_results=4 "
              << "qc_failed=2 archive=" << manifest.storage_path << std::endl;
}

TEST(TaskExecutorTest, ReportsPartialRowFailureWithoutDroppingValidRecords) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "partial.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001\n"
        "ST002,DV002,2026-08-08 08:00:00,44\n");
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(executable_task(tree.inbox())));

    tree.track_archive(client.manifests.front().files.front().storage_path);
    const auto& report = client.reports.front();
    EXPECT_EQ(report.status, labbridge::core::TaskRunStatus::Failed);
    EXPECT_EQ(report.items_total, 2);
    EXPECT_EQ(report.items_success, 1);
    EXPECT_EQ(report.items_failed, 1);
    ASSERT_EQ(report.parsed_records.size(), 1U);
    EXPECT_NE(report.error_summary.find("line 2"), std::string::npos);
    std::cout << "partial_failure total=" << report.items_total
              << " success=" << report.items_success
              << " failed=" << report.items_failed
              << " error_summary=" << report.error_summary << std::endl;
}

TEST(TaskExecutorTest, EmptyDirectorySkipsManifestAndReportsZeroSuccess) {
    TemporaryExecutionTree tree;
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(executable_task(tree.inbox())));

    EXPECT_EQ(
        client.events,
        (std::vector<std::string>{"start", "report"}));
    ASSERT_EQ(client.reports.size(), 1U);
    EXPECT_EQ(
        client.reports.front().status,
        labbridge::core::TaskRunStatus::Succeeded);
    EXPECT_EQ(client.reports.front().items_total, 0);
}

TEST(TaskExecutorTest, ManifestIdMismatchProducesOnlyFailedTerminalReport) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "mismatch.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n");
    FakeExecutionClient client;
    client.return_wrong_manifest_count = true;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(executable_task(tree.inbox())));

    tree.track_archive(client.manifests.front().files.front().storage_path);
    EXPECT_EQ(
        client.events,
        (std::vector<std::string>{"start", "manifest", "report"}));
    ASSERT_EQ(client.reports.size(), 1U);
    EXPECT_EQ(
        client.reports.front().status,
        labbridge::core::TaskRunStatus::Failed);
    EXPECT_TRUE(client.reports.front().parsed_records.empty());
    EXPECT_NE(
        client.reports.front().error_summary.find("raw_file_ids count"),
        std::string::npos);
}

TEST(TaskExecutorTest, AcknowledgedSuccessSkipsUnchangedFileInSameProcess) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "unchanged.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n");
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(executable_task(tree.inbox())));
    tree.track_archive(client.manifests.front().files.front().storage_path);
    executor.execute(scheduled(executable_task(tree.inbox())));

    ASSERT_EQ(client.starts.size(), 2U);
    ASSERT_EQ(client.manifests.size(), 1U);
    ASSERT_EQ(client.reports.size(), 2U);
    EXPECT_EQ(client.reports.back().items_total, 0);
    executor.forget_task("30");
    executor.execute(scheduled(executable_task(tree.inbox())));
    tree.track_archive(client.manifests.back().files.front().storage_path);
    ASSERT_EQ(client.manifests.size(), 2U);
    std::cout << "dedup first_manifest_files=1 second_manifest_files=0 "
              << "after_forget_manifest_files=1" << std::endl;
}

TEST(TaskExecutorTest, FailedReportDoesNotMarkFileAsProcessed) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "retry.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n");
    FakeExecutionClient client;
    client.fail_next_report = true;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    EXPECT_THROW(
        executor.execute(scheduled(executable_task(tree.inbox()))),
        std::runtime_error);
    tree.track_archive(client.manifests.front().files.front().storage_path);
    executor.execute(scheduled(executable_task(tree.inbox())));
    tree.track_archive(client.manifests.back().files.front().storage_path);

    EXPECT_EQ(client.manifests.size(), 2U);
}

TEST(TaskExecutorTest, RejectsRemotePathOutsideLocalAllowListAfterStart) {
    TemporaryExecutionTree tree;
    FakeExecutionClient client;
    auto task = executable_task(tree.inbox());
    task.data_source.config_json =
        "{\"root_path\":\"/tmp/not-allowed\",\"extension\":\".csv\"}";
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(std::move(task)));

    EXPECT_EQ(
        client.events,
        (std::vector<std::string>{"start", "report"}));
    EXPECT_EQ(
        client.reports.front().status,
        labbridge::core::TaskRunStatus::Failed);
    EXPECT_NE(
        client.reports.front().error_summary.find("outside allowed"),
        std::string::npos);
}

TEST(TaskExecutorTest, ArchiveFailureProducesFailedTerminalReport) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "archive-failure.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n");
    tree.block_work_directory_with_file();
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(executable_task(tree.inbox())));

    EXPECT_EQ(
        client.events,
        (std::vector<std::string>{"start", "report"}));
    EXPECT_EQ(
        client.reports.front().status,
        labbridge::core::TaskRunStatus::Failed);
    EXPECT_EQ(client.reports.front().items_failed, 1);
    EXPECT_FALSE(client.reports.front().error_summary.empty());
}

TEST(TaskExecutorTest, InvalidHeaderFailsFileWithoutPublishingRecords) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "bad-header.csv",
        "site,instrument,time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n");
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(executable_task(tree.inbox())));

    tree.track_archive(client.manifests.front().files.front().storage_path);
    EXPECT_EQ(
        client.reports.front().status,
        labbridge::core::TaskRunStatus::Failed);
    EXPECT_TRUE(client.reports.front().parsed_records.empty());
    EXPECT_NE(
        client.reports.front().error_summary.find("header"),
        std::string::npos);
}

TEST(TaskExecutorTest, FingerprintCapacityEvictsOldestSuccessfulFile) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "a.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,41\n");
    tree.write_csv(
        "b.csv",
        "station_code,device_code,record_time,value\n"
        "ST002,DV002,2026-08-08 08:00:00,42\n");
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now(), 1};

    executor.execute(scheduled(executable_task(tree.inbox())));
    for (const auto& file : client.manifests.front().files) {
        tree.track_archive(file.storage_path);
    }
    executor.execute(scheduled(executable_task(tree.inbox())));
    for (const auto& file : client.manifests.back().files) {
        tree.track_archive(file.storage_path);
    }

    ASSERT_EQ(client.manifests.size(), 2U);
    EXPECT_EQ(client.manifests.front().files.size(), 2U);
    ASSERT_EQ(client.manifests.back().files.size(), 1U);
    EXPECT_EQ(client.manifests.back().files.front().original_name, "a.csv");
}

TEST(TaskExecutorTest, BoundsErrorSummaryDetailsAndBytes) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "many-errors.csv",
        "station_code,device_code,record_time,value\n"
        "S1,D1\n"
        "S2,D2\n"
        "S3,D3\n"
        "S4,D4\n"
        "S5,D5\n"
        "S6,D6\n"
        "S7,D7\n");
    FakeExecutionClient client;
    labbridge::agent::TaskExecutor executor{
        client, tree.work(), {tree.inbox()}, fixed_now()};

    executor.execute(scheduled(executable_task(tree.inbox())));

    tree.track_archive(client.manifests.front().files.front().storage_path);
    const auto& report = client.reports.front();
    EXPECT_EQ(report.items_total, 7);
    EXPECT_EQ(report.items_failed, 7);
    EXPECT_LE(report.error_summary.size(), 512U);
    EXPECT_NE(report.error_summary.find("7 error(s)"), std::string::npos);
    EXPECT_NE(
        report.error_summary.find("2 additional error(s) omitted"),
        std::string::npos);
}
TEST(TaskExecutorTest, RecoversFrozenReportAndPersistsDedupAcrossObjects) {
    TemporaryExecutionTree tree;
    tree.write_csv(
        "reliable.csv",
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n");
    FakeExecutionClient client;
    const auto database = (tree.work().parent_path() / "queue.db").string();
    const auto task = executable_task(tree.inbox());
    {
        labbridge::agent::AgentQueueStore store{
            database, task.node_code, 10, 10};
        labbridge::agent::TaskExecutor executor{
            client, store, tree.work(), {tree.inbox()}, fixed_now()};
        client.fail_next_report = true;
        EXPECT_THROW(executor.execute(scheduled(task)), std::runtime_error);
        ASSERT_EQ(store.recover_jobs().size(), 1U);
        EXPECT_EQ(store.recover_jobs().front().stage, "report_pending");
        tree.track_archive(
            store.recover_jobs().front().manifest_request.files.front().storage_path);
    }
    const auto frozen_report = client.reports.front();
    {
        labbridge::agent::AgentQueueStore store{
            database, task.node_code, 10, 10};
        labbridge::agent::TaskExecutor executor{
            client, store, tree.work(), {tree.inbox()}, fixed_now()};
        executor.recover_pending_jobs();
        EXPECT_EQ(store.pending_job_count(), 0U);
        EXPECT_EQ(client.reports.back().idempotency_key,
                  frozen_report.idempotency_key);
        executor.execute(scheduled(task));
        EXPECT_EQ(client.manifests.size(), 1U);
        EXPECT_EQ(client.reports.back().items_total, 0);
    }
    std::filesystem::remove(database);
    std::filesystem::remove(database + "-wal");
    std::filesystem::remove(database + "-shm");
    std::cout << "recovery stage=report_pending replayed_report=1 "
              << "pending_jobs=0 next_run_manifest_files=0" << std::endl;
}

}  // namespace
