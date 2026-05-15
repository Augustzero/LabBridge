#pragma once

#include "labbridge/agent/collector.h"
#include "labbridge/core/filesystem.h"

#include <string>

namespace labbridge::agent {

class LocalDirCollector final : public ICollector {
public:
    LocalDirCollector(labbridge::core::fs::path root_path, std::string extension_filter);

    CollectResult collect(const TaskContext& context) override;

private:
    labbridge::core::fs::path root_path_;
    std::string extension_filter_;
};

}  // namespace labbridge::agent
