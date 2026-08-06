#include "labbridge/agent/collectors/local_dir_collector.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()) +
            "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        root_ = std::filesystem::temp_directory_path() /
                ("labbridge-local-collector-" + suffix);
        if (!std::filesystem::create_directory(root_)) {
            throw std::runtime_error("failed to create temporary directory");
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        for (auto iterator = files_.rbegin(); iterator != files_.rend();
             ++iterator) {
            std::filesystem::remove(*iterator, ignored);
        }
        for (auto iterator = directories_.rbegin();
             iterator != directories_.rend(); ++iterator) {
            std::filesystem::remove(*iterator, ignored);
        }
        std::filesystem::remove(root_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path create_directory(const std::string& name) {
        const auto path = root_ / name;
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("failed to create nested directory");
        }
        directories_.push_back(path);
        return path;
    }

    std::filesystem::path create_file(const std::string& name,
                                      const std::string& contents = {}) {
        const auto path = root_ / name;
        std::ofstream output{path};
        if (!output.is_open()) {
            throw std::runtime_error("failed to create temporary file");
        }
        output << contents;
        if (!output.good()) {
            throw std::runtime_error("failed to write temporary file");
        }
        files_.push_back(path);
        return path;
    }

    std::filesystem::path create_nested_file(
        const std::filesystem::path& directory,
        const std::string& name) {
        const auto path = directory / name;
        std::ofstream output{path};
        if (!output.is_open()) {
            throw std::runtime_error("failed to create nested file");
        }
        files_.push_back(path);
        return path;
    }

    const std::filesystem::path& root() const {
        return root_;
    }

private:
    std::filesystem::path root_;
    std::vector<std::filesystem::path> files_;
    std::vector<std::filesystem::path> directories_;
};

TEST(LocalDirCollectorTest, ReturnsFailureForMissingDirectory) {
    TemporaryDirectory temporary;
    labbridge::agent::LocalDirCollector collector{
        temporary.root() / "missing",
        ".csv"};

    const auto result = collector.collect({});

    EXPECT_FALSE(result.status.ok);
    EXPECT_FALSE(result.status.message.empty());
    EXPECT_TRUE(result.items.empty());
}

TEST(LocalDirCollectorTest, ReturnsFailureWhenRootIsAFile) {
    TemporaryDirectory temporary;
    const auto file = temporary.create_file("root.csv");
    labbridge::agent::LocalDirCollector collector{file, ".csv"};

    const auto result = collector.collect({});

    EXPECT_FALSE(result.status.ok);
    EXPECT_FALSE(result.status.message.empty());
    EXPECT_TRUE(result.items.empty());
}

TEST(LocalDirCollectorTest, CollectsMatchingFilesAndSkipsNestedFiles) {
    TemporaryDirectory temporary;
    temporary.create_file("observation.csv", "header\n");
    temporary.create_file("notes.txt", "ignored\n");
    const auto nested = temporary.create_directory("nested");
    temporary.create_nested_file(nested, "nested.csv");
    labbridge::agent::LocalDirCollector collector{temporary.root(), ".csv"};

    const auto result = collector.collect({});

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_EQ(result.items.size(), 1U);
    EXPECT_EQ(result.items.front().original_name, "observation.csv");
    EXPECT_EQ(result.items.front().source_mtime, "pending");
}

TEST(LocalDirCollectorTest, ConvertsFilesystemErrorsToFailureStatus) {
    TemporaryDirectory temporary;
    const auto too_long_path = temporary.root() / std::string(5000, 'x');
    labbridge::agent::LocalDirCollector collector{too_long_path, ".csv"};
    labbridge::agent::CollectResult result;

    EXPECT_NO_THROW(result = collector.collect({}));

    EXPECT_FALSE(result.status.ok);
    EXPECT_FALSE(result.status.message.empty());
    EXPECT_TRUE(result.items.empty());
}

}  // namespace
