#include "labbridge/agent/storage/agent_queue_store.h"
#include "labbridge/core/filesystem.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <iostream>

namespace {

class TempDirectory final {
public:
    TempDirectory()
        : path_(labbridge::core::fs::temp_directory_path() /
                ("labbridge-queue-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()))) {
        labbridge::core::fs::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        labbridge::core::fs::remove_all(path_, error);
    }

    const labbridge::core::fs::path& path() const {
        return path_;
    }

private:
    labbridge::core::fs::path path_;
};

labbridge::core::TaskConfig task_config() {
    return {
        "42",
        "node-1",
        "7",
        "CSV",
        "local_file_import",
        "* * * * *",
        "csv_observation",
        "basic",
        true,
        {
            "7",
            "node-1",
            labbridge::core::SourceType::LocalDirectory,
            "Inbox",
            R"({"root_path":"/inbox"})",
        },
        {},
    };
}

labbridge::agent::StartTaskRunRequest start_request() {
    return {
        "node-1",
        "42",
        "execution-1",
        "2026-08-12T00:00:00Z",
        "2026-08-12T00:00:01Z",
        "scheduled",
    };
}

labbridge::agent::PendingFilePlan file_plan(int ordinal,
                                             std::string fingerprint) {
    return {
        ordinal,
        "/inbox/sample.csv",
        "sample.csv",
        "2026-08-12T00:00:00Z",
        42,
        "hash",
        std::move(fingerprint),
        "/archive/sample.csv",
    };
}

}  // namespace

TEST(AgentQueueStoreTest, ReopensAndRecoversJobDeliveryAndFilePlan) {
    TempDirectory temp;
    const auto database_path = (temp.path() / "queue.db").string();

    {
        labbridge::agent::AgentQueueStore store{
            database_path, "node-1", 10};
        EXPECT_TRUE(store.begin_job(task_config(), start_request()));
        EXPECT_FALSE(store.begin_job(task_config(), start_request()));
        store.save_file_plan(
            "execution-1", {file_plan(0, "fingerprint")});
        EXPECT_EQ(store.pending_job_count(), 1U);
    }

    {
        labbridge::agent::AgentQueueStore store{
            database_path, "node-1", 10};
        const auto jobs = store.recover_jobs();

        ASSERT_EQ(jobs.size(), 1U);
        EXPECT_EQ(jobs.front().task.id, "42");
        EXPECT_EQ(jobs.front().start_request.execution_key, "execution-1");
        ASSERT_EQ(jobs.front().files.size(), 1U);
        EXPECT_EQ(jobs.front().files.front().archive_path,
                  "/archive/sample.csv");

        std::cout << "queue_reopen execution_key="
                  << jobs.front().execution_key
                  << " stage=" << jobs.front().stage
                  << " file=" << jobs.front().files.front().original_name
                  << std::endl;
    }

    EXPECT_THROW(
        labbridge::agent::AgentQueueStore(database_path, "other-node", 10),
        labbridge::agent::AgentQueueError);
}

TEST(AgentQueueStoreTest, RollsBackWholeFilePlanAndEnforcesCapacity) {
    TempDirectory temp;
    labbridge::agent::AgentQueueStore store{
        (temp.path() / "queue.db").string(), "node-1", 1};
    EXPECT_TRUE(store.begin_job(task_config(), start_request()));

    EXPECT_THROW(
        store.save_file_plan(
            "execution-1",
            {
                file_plan(0, "same-fingerprint"),
                file_plan(1, "same-fingerprint"),
            }),
        labbridge::agent::AgentQueueError);
    EXPECT_TRUE(store.recover_jobs().front().files.empty());

    auto second_request = start_request();
    second_request.execution_key = "execution-2";
    second_request.scheduled_for = "2026-08-12T00:01:00Z";
    EXPECT_THROW(
        store.begin_job(task_config(), second_request),
        labbridge::agent::AgentQueueError);
}

TEST(AgentQueueStoreTest, RejectsUnsupportedOrIncompleteSchema) {
    TempDirectory temp;
    const auto database_path = (temp.path() / "queue.db").string();

    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(database_path.c_str(), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database,
                           "PRAGMA user_version=2",
                           nullptr,
                           nullptr,
                           nullptr),
              SQLITE_OK);
    sqlite3_close(database);

    EXPECT_THROW(
        labbridge::agent::AgentQueueStore(database_path, "node-1", 10),
        labbridge::agent::AgentQueueError);
}
