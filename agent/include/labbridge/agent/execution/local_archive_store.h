#pragma once

#include "labbridge/agent/collectors/collector.h"
#include "labbridge/core/filesystem.h"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace labbridge::agent {

struct LocalFileMetadata {
    labbridge::core::fs::path source_path;
    std::string original_name;
    std::string file_hash;
    long long size_bytes{0};
    std::string source_mtime;
    std::string fingerprint;
};

struct ArchivedLocalFile {
    LocalFileMetadata source;
    labbridge::core::fs::path archive_path;
};

// 归档目标已存在且与持久化指纹不一致：证据可能已被外部改动，
// 作业必须停留 requires_attention 等待人工处理，不能自动重试或覆盖。
class ArchiveConflictError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LocalArchiveStore {
public:
    explicit LocalArchiveStore(labbridge::core::fs::path work_dir);

    LocalFileMetadata inspect(const CollectedItem& item) const;
    ArchivedLocalFile archive(const std::string& task_id,
                              const std::string& task_run_id,
                              std::size_t ordinal,
                              const LocalFileMetadata& source) const;
    labbridge::core::fs::path plan_archive_path(
        const std::string& task_id,
        const std::string& task_run_id,
        std::size_t ordinal,
        const std::string& original_name) const;
    ArchivedLocalFile recover_archive(
        const LocalFileMetadata& source,
        const labbridge::core::fs::path& archive_path) const;

    const labbridge::core::fs::path& work_dir() const noexcept;

private:
    labbridge::core::fs::path work_dir_;
};

}  // namespace labbridge::agent
