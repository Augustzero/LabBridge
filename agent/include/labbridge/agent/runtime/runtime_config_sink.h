#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/core/models.h"

#include <vector>

namespace labbridge::agent {

class IRuntimeConfigSink {
public:
    virtual ~IRuntimeConfigSink() = default;
    // 任务配置与人工待执行候选一起发布；字段缺席按空列表处理。
    virtual void replace_config(
        std::vector<labbridge::core::TaskConfig> tasks,
        std::vector<PendingExecution> pending_executions) = 0;
};

}  // namespace labbridge::agent
