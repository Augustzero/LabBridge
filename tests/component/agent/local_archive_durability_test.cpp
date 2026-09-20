#include "labbridge/agent/execution/local_archive_store.h"
#include "labbridge/agent/execution/sha256.h"
#include <gtest/gtest.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
std::string fail_path;
int injected_error = EIO;
std::vector<std::string> synced_paths;
}
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    char path[4096];
    const auto name = "/proc/self/fd/" + std::to_string(fd);
    const auto size = ::readlink(name.c_str(), path, sizeof(path) - 1);
    if (size < 0) return __real_fsync(fd);
    path[size] = '\0';
    synced_paths.emplace_back(path);
    if (fail_path == path) {
        fail_path.clear();
        errno = injected_error;
        return -1;
    }
    return __real_fsync(fd);
}
namespace {
class ArchiveDurabilityTest : public testing::Test {
protected:
    void SetUp() override {
        char pattern[] = "/tmp/labbridge-archive-durability-XXXXXX";
        const auto directory = ::mkdtemp(pattern);
        ASSERT_NE(directory, nullptr);
        root = directory;
        std::ofstream(root / "input.csv") << "sample\n";
        metadata.source_path = root / "input.csv";
        metadata.original_name = "input.csv";
        metadata.file_hash = labbridge::agent::sha256_file_hex(metadata.source_path);
        metadata.size_bytes = 7;
        fail_path.clear();
        synced_paths.clear();
        injected_error = EIO;
    }
    void TearDown() override {
        fail_path.clear();
        // 故障现场保留在独立临时目录，便于排查，不做批量删除。
        RecordProperty("artifact_directory", root.string());
    }
    void expect_chain_synced(const std::filesystem::path& file) {
        auto directory = file.parent_path();
        for (;;) {
            EXPECT_NE(std::find(synced_paths.begin(), synced_paths.end(), directory.string()),
                      synced_paths.end()) << directory;
            const auto parent = directory.parent_path();
            if (parent == directory) break;
            directory = parent;
        }
    }
    std::filesystem::path root;
    labbridge::agent::LocalFileMetadata metadata;
};
TEST_F(ArchiveDurabilityTest, RecoversAncestorsLeftByFailedSync) {
    labbridge::agent::LocalArchiveStore store{root / "work"};
    fail_path = (root / "work").string();
    EXPECT_THROW(store.archive("task", "run1", 1, metadata), std::runtime_error);
    ASSERT_TRUE(fail_path.empty()) << "injection was not reached";
    synced_paths.clear();
    const auto archived = store.archive("task", "run2", 1, metadata);
    expect_chain_synced(archived.archive_path);
    EXPECT_EQ(labbridge::agent::sha256_file_hex(archived.archive_path), metadata.file_hash);
}
TEST_F(ArchiveDurabilityTest, ExistingArchiveStillRequiresAllAncestors) {
    labbridge::agent::LocalArchiveStore store{root / "work"};
    const auto path = store.plan_archive_path("task", "run1", 1, metadata.original_name);
    fail_path = (root / "work").string();
    EXPECT_THROW(store.archive("task", "run1", 1, metadata), std::runtime_error);
    ASSERT_TRUE(std::filesystem::exists(path));
    synced_paths.clear();
    store.recover_archive(metadata, path);
    expect_chain_synced(path);
    EXPECT_EQ(synced_paths.front(), path.string());
}
TEST_F(ArchiveDurabilityTest, InterruptedSyncIsRetried) {
    labbridge::agent::LocalArchiveStore store{root / "work"};
    fail_path = (root / "work").string();
    injected_error = EINTR;
    const auto archived = store.archive("task", "run1", 1, metadata);
    EXPECT_EQ(std::count(synced_paths.begin(), synced_paths.end(), (root / "work").string()), 2);
    expect_chain_synced(archived.archive_path);
}
}  // namespace
