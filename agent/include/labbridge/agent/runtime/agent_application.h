#pragma once

#include "labbridge/agent/runtime/agent_runtime.h"
#include "labbridge/agent/scheduler/task_scheduler.h"

#include <atomic>

namespace labbridge::agent {

class AgentApplication final {
public:
    AgentApplication(AgentRuntime& runtime, TaskScheduler& scheduler);

    PulledAgentConfig run();
    void request_stop() noexcept;

private:
    AgentRuntime& runtime_;
    TaskScheduler& scheduler_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace labbridge::agent
