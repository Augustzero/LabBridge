#pragma once

#include "labbridge/core/result.h"

#include <string>
#include <vector>

namespace labbridge::agent {

struct TaskContext {
    std::string task_id;
    std::string node_code;
    std::string source_config_json;
};

struct CollectedItem {
    std::string local_path;
    std::string original_name;
    std::string source_mtime;
};

struct CollectResult {
    labbridge::core::Status status;
    std::vector<CollectedItem> items;
};

class ICollector {
public:
    virtual ~ICollector() = default;
    virtual CollectResult collect(const TaskContext& context) = 0;
};

}  // namespace labbridge::agent

