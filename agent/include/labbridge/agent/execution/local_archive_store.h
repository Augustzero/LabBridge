#pragma once

#include "labbridge/agent/collectors/collector.h"
#include "labbridge/core/filesystem.h"

#include <cstddef>
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

class LocalArchiveStore {
public:
    explicit LocalArchiveStore(labbridge::core::fs::path work_dir);

    LocalFileMetadata inspect(const CollectedItem& item) const;
    ArchivedLocalFile archive(const std::string& task_id,
                              const std::string& task_run_id,
                              std::size_t ordinal,
                              const LocalFileMetadata& source) const;

    const labbridge::core::fs::path& work_dir() const noexcept;

private:
    labbridge::core::fs::path work_dir_;
};

}  // namespace labbridge::agent
