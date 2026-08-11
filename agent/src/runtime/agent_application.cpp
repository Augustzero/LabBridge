#include "labbridge/agent/runtime/agent_application.h"

#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace labbridge::agent {

AgentApplication::AgentApplication(AgentRuntime& runtime,
                                   TaskScheduler& scheduler)
    : runtime_(runtime), scheduler_(scheduler) {}

PulledAgentConfig AgentApplication::run() {
    // 初始握手快照必须在线程启动前发布，避免首个时间槽看到空配置。
    runtime_.publish_initial_config();

    std::mutex exception_mutex;
    std::exception_ptr worker_exception;
    std::thread worker{[this, &exception_mutex, &worker_exception] {
        try {
            scheduler_.run();
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock{exception_mutex};
                worker_exception = std::current_exception();
            }
            runtime_.request_stop();
        }
    }};

    std::optional<PulledAgentConfig> final_config;
    std::exception_ptr runtime_exception;
    try {
        final_config = runtime_.run();
    } catch (...) {
        runtime_exception = std::current_exception();
    }

    request_stop();
    worker.join();

    if (runtime_exception) {
        std::rethrow_exception(runtime_exception);
    }
    {
        std::lock_guard<std::mutex> lock{exception_mutex};
        if (worker_exception) {
            std::rethrow_exception(worker_exception);
        }
    }
    return std::move(*final_config);
}

void AgentApplication::request_stop() noexcept {
    const bool was_stopped =
        stop_requested_.exchange(true, std::memory_order_acq_rel);
    if (!was_stopped) {
        runtime_.request_stop();
        scheduler_.request_stop();
    }
}

}  // namespace labbridge::agent
