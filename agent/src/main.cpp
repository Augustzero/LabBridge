#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/agent/bootstrap/process_signal_monitor.h"
#include "labbridge/agent/bootstrap/startup_handshake.h"
#include "labbridge/agent/execution/task_executor.h"
#include "labbridge/agent/runtime/agent_application.h"
#include "labbridge/agent/runtime/agent_runtime.h"
#include "labbridge/agent/scheduler/task_scheduler.h"
#include "labbridge/core/logging.h"
#include "labbridge/core/version.h"

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kComponent = "agent";

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = argc > 1 ? argv[1] : "deploy/env/agent.example.yaml";

    try {
        labbridge::core::log_info(kComponent, "starting LabBridge agent");
        labbridge::core::log_info(kComponent, labbridge::core::kVersion);
        labbridge::core::log_info(kComponent, "config path: " + config_path);

        const auto config = labbridge::agent::load_agent_config(config_path);
        labbridge::agent::ControlPlaneClient client{
            config.server_url,
            config.request_timeout};
        const auto remote_config =
            labbridge::agent::perform_startup_handshake(client, config.node);

        labbridge::core::log_info(
            kComponent,
            "startup handshake completed; enabled tasks: " +
                std::to_string(remote_config.tasks.size()));

        std::vector<labbridge::core::fs::path> allowed_local_roots;
        allowed_local_roots.reserve(config.allowed_local_roots.size());
        for (const auto& root : config.allowed_local_roots) {
            allowed_local_roots.emplace_back(root);
        }

        labbridge::agent::SystemRuntimeTimeSource runtime_time_source;
        labbridge::agent::TaskExecutor executor{
            client, config.work_dir, std::move(allowed_local_roots)};
        labbridge::agent::SystemSchedulerTimeSource scheduler_time_source;
        labbridge::agent::TaskScheduler scheduler{
            executor, scheduler_time_source};
        labbridge::agent::AgentRuntime runtime{
            config.node,
            config.heartbeat_interval,
            config.config_poll_interval,
            client,
            remote_config,
            runtime_time_source,
            &scheduler};
        labbridge::agent::AgentApplication application{runtime, scheduler};
        labbridge::agent::ProcessSignalMonitor signal_monitor{[&application] {
            labbridge::core::log_info(kComponent, "stop requested");
            application.request_stop();
        }};

        labbridge::core::log_info(
            kComponent, "runtime control and task loops started");
        const auto final_config = application.run();
        labbridge::core::log_info(
            kComponent,
            "runtime control and task loops stopped; enabled tasks: " +
                std::to_string(final_config.tasks.size()));
        return 0;
    } catch (const std::exception& error) {
        labbridge::core::log_error(kComponent, error.what());
        return 1;
    } catch (...) {
        labbridge::core::log_error(kComponent, "unknown startup failure");
        return 1;
    }
}
