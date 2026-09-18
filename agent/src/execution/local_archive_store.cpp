#include "labbridge/agent/execution/local_archive_store.h"
#include "labbridge/agent/execution/sha256.h"
#include "labbridge/core/utc_time.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace labbridge::agent {
namespace {

std::atomic<unsigned long long> temporary_sequence{0};

std::string format_file_time(labbridge::core::fs::file_time_type value) {
    const auto system_time = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
        value - labbridge::core::fs::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return labbridge::core::format_utc_timestamp(system_time);
}

bool is_safe_segment(const std::string& value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    for (const unsigned char character : value) {
        if (std::isalnum(character) == 0 && character != '-' &&
            character != '_' && character != '.') {
            return false;
        }
    }
    return true;
}

std::string sanitized_filename(const std::string& original_name) {
    std::string result;
    result.reserve(original_name.size());
    for (const unsigned char character : original_name) {
        if (std::isalnum(character) != 0 || character == '-' ||
            character == '_' || character == '.') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('_');
        }
    }
    if (result.empty() || result == "." || result == "..") {
        return "file";
    }
    return result;
}

// 只为落盘同步服务的 fd RAII：打开失败直接抛错，析构必关，
// 省得每个错误分支都记得 close。不做通用文件封装。
class SyncDescriptor {
public:
    SyncDescriptor(labbridge::core::fs::path path, int flags)
        : path_{std::move(path)},
          fd_{::open(path_.c_str(), flags)} {
        if (fd_ < 0) {
            throw std::runtime_error(
                "failed to open " + path_.string() +
                " for sync: " + std::strerror(errno));
        }
    }

    ~SyncDescriptor() { ::close(fd_); }

    SyncDescriptor(const SyncDescriptor&) = delete;
    SyncDescriptor& operator=(const SyncDescriptor&) = delete;

    void sync() const {
        // Agent 停止时会收到信号，fsync 被信号打断返回 EINTR 属正常，接着重试。
        while (::fsync(fd_) != 0) {
            if (errno != EINTR) {
                throw std::runtime_error(
                    "failed to sync " + path_.string() + ": " +
                    std::strerror(errno));
            }
        }
    }

private:
    labbridge::core::fs::path path_;
    int fd_;
};

// 把文件已写入的内容压到稳定存储。只读 fd 调 fsync 一样能把脏页刷下去，
// 这里本来也只做同步，不需要写权限。
void sync_file(const labbridge::core::fs::path& path) {
    SyncDescriptor descriptor{path, O_RDONLY};
    descriptor.sync();
}

// 同步目录项。rename/mkdir 改的目录项先挂在内存里，不同步所在目录，
// 掉电后名字可能就没了，所以每次改名后都要补这一下。
void sync_directory(const labbridge::core::fs::path& directory) {
    SyncDescriptor descriptor{directory, O_RDONLY | O_DIRECTORY};
    descriptor.sync();
}

// 建归档层级目录，并让新建的每一层都真正落盘：新目录自己 fsync 一次，
// 它的父目录再 fsync 一次（父目录多了个名字也要持久化）。跳过已存在的层级，
// 这样日常运行的额外开销就是 rename 后那一次目录同步。
void create_and_sync_directories(const labbridge::core::fs::path& directory) {
    std::vector<labbridge::core::fs::path> missing;
    labbridge::core::fs::path prefix;
    for (const auto& part : directory) {
        prefix /= part;
        if (labbridge::core::fs::exists(prefix)) {
            continue;
        }
        missing.push_back(prefix);
    }
    labbridge::core::fs::create_directories(directory);
    for (const auto& created : missing) {
        sync_directory(created);
        sync_directory(created.parent_path());
    }
}

}  // namespace

LocalArchiveStore::LocalArchiveStore(labbridge::core::fs::path work_dir)
    : work_dir_(labbridge::core::fs::weakly_canonical(
          labbridge::core::fs::absolute(std::move(work_dir)))) {}

LocalFileMetadata LocalArchiveStore::inspect(const CollectedItem& item) const {
    LocalFileMetadata metadata;
    const labbridge::core::fs::path input_path{item.local_path};
    const auto status = labbridge::core::fs::symlink_status(input_path);
    metadata.source_path =
        labbridge::core::fs::weakly_canonical(input_path);
    if (labbridge::core::fs::is_symlink(status) ||
        !labbridge::core::fs::is_regular_file(status)) {
        throw std::runtime_error("source is not a regular non-symlink file");
    }

    const auto size = labbridge::core::fs::file_size(metadata.source_path);
    if (size > static_cast<std::uintmax_t>(
                   std::numeric_limits<long long>::max())) {
        throw std::runtime_error("source file is too large");
    }
    metadata.size_bytes = static_cast<long long>(size);
    metadata.original_name = item.original_name;
    metadata.source_mtime =
        format_file_time(labbridge::core::fs::last_write_time(metadata.source_path));
    metadata.file_hash = sha256_file_hex(metadata.source_path);
    metadata.fingerprint = metadata.source_path.string() + "\n" +
                           std::to_string(metadata.size_bytes) + "\n" +
                           metadata.source_mtime + "\n" + metadata.file_hash;
    return metadata;
}

ArchivedLocalFile LocalArchiveStore::archive(
    const std::string& task_id,
    const std::string& task_run_id,
    std::size_t ordinal,
    const LocalFileMetadata& source) const {
    if (!is_safe_segment(task_id) || !is_safe_segment(task_run_id)) {
        throw std::invalid_argument("task and task run IDs must be safe path segments");
    }

    const auto destination =
        plan_archive_path(task_id, task_run_id, ordinal, source.original_name);
    const auto directory = destination.parent_path();
    create_and_sync_directories(directory);
    if (labbridge::core::fs::exists(destination)) {
        throw ArchiveConflictError("archive destination already exists");
    }

    const auto filename = destination.filename().string();
    const auto temporary =
        directory /
        (filename + ".tmp-" +
         std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed)));
        // 先写同目录临时文件并校验内容，再同步、再 rename，
        // manifest 永远不会指向半写入的归档证据。
    try {
        labbridge::core::fs::copy_file(
            source.source_path, temporary,
            labbridge::core::fs::copy_options::none);
        const auto archive_size = labbridge::core::fs::file_size(temporary);
        const auto archive_hash = sha256_file_hex(temporary);
        if (archive_size != static_cast<std::uintmax_t>(source.size_bytes) ||
            archive_hash != source.file_hash) {
            throw std::runtime_error("source changed while it was being archived");
        }
        // 内容先落盘再改名：否则掉电后可能出现“名字在了、内容还是空的”。
        sync_file(temporary);
        labbridge::core::fs::rename(temporary, destination);
        // rename 本身不同步目录项，这里补上，改名才算真正定下来。
        sync_directory(directory);
    } catch (...) {
        std::error_code ignored;
        labbridge::core::fs::remove(temporary, ignored);
        throw;
    }

    return {source, labbridge::core::fs::weakly_canonical(destination)};
}
ArchivedLocalFile LocalArchiveStore::recover_archive(
    const LocalFileMetadata& source,
    const labbridge::core::fs::path& archive_path) const {
    if (labbridge::core::fs::exists(archive_path)) {
        if (!labbridge::core::fs::is_regular_file(archive_path) ||
            labbridge::core::fs::file_size(archive_path) !=
                static_cast<std::uintmax_t>(source.size_bytes) ||
            sha256_file_hex(archive_path) != source.file_hash) {
            throw ArchiveConflictError(
                "persisted archive conflicts with expected evidence");
        }
        // 上次归档可能在同步完成前就被打断（比如进程被杀后重启）。
        // 文件还在且内容对，只说明页缓存里有它，不等于已经落盘，
        // 所以恢复分支也要把文件和目录项补同步，再对外宣布归档成功。
        sync_file(archive_path);
        sync_directory(archive_path.parent_path());
        return {source, labbridge::core::fs::weakly_canonical(archive_path)};
    }
    const auto filename = archive_path.filename().string();
    const auto separator = filename.find('-');
    if (separator == std::string::npos) {
        throw std::runtime_error("invalid persisted archive path");
    }
    const auto parent = archive_path.parent_path();
    return archive(
        parent.parent_path().filename().string(),
        parent.filename().string(),
        static_cast<std::size_t>(std::stoull(filename.substr(0, separator))),
        source);
}
labbridge::core::fs::path LocalArchiveStore::plan_archive_path(
    const std::string& task_id,
    const std::string& task_run_id,
    std::size_t ordinal,
    const std::string& original_name) const {
    if (!is_safe_segment(task_id) || !is_safe_segment(task_run_id) ||
        ordinal == 0) {
        throw std::invalid_argument(
            "task, task run and ordinal must form a safe archive path");
    }
    return work_dir_ / "archive" / task_id / task_run_id /
           (std::to_string(ordinal) + "-" +
            sanitized_filename(original_name));
}

const labbridge::core::fs::path& LocalArchiveStore::work_dir() const noexcept {
    return work_dir_;
}

}  // namespace labbridge::agent
