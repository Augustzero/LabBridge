#include "labbridge/agent/local_dir_collector.h"

#include <chrono>
#include <utility>

namespace labbridge::agent {

LocalDirCollector::LocalDirCollector(labbridge::core::fs::path root_path, std::string extension_filter)
    : root_path_(std::move(root_path)), extension_filter_(std::move(extension_filter)) {}

CollectResult LocalDirCollector::collect(const TaskContext&) {
    CollectResult result;

    if (!labbridge::core::fs::exists(root_path_)) {
        result.status = labbridge::core::Status::failure("local directory does not exist");
        return result;
    }

    if (!labbridge::core::fs::is_directory(root_path_)) {
        result.status = labbridge::core::Status::failure("local path is not a directory");
        return result;
    }

    for (const auto& entry : labbridge::core::fs::directory_iterator(root_path_)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (!extension_filter_.empty() && entry.path().extension().string() != extension_filter_) {
            continue;
        }

        CollectedItem item;
        item.local_path = entry.path().string();
        item.original_name = entry.path().filename().string();
        item.source_mtime = "pending";
        result.items.push_back(std::move(item));
    }

    result.status = labbridge::core::Status::success();
    return result;
}

}  // namespace labbridge::agent
