#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/agent/bootstrap/process_signal_monitor.h"
#include "labbridge/agent/bootstrap/startup_handshake.h"
#include "labbridge/agent/execution/reliable_delivery_client.h"
#include "labbridge/agent/execution/task_executor.h"
#include "labbridge/agent/runtime/agent_application.h"
#include "labbridge/agent/runtime/agent_runtime.h"
#include "labbridge/agent/scheduler/task_scheduler.h"
#include "labbridge/agent/storage/agent_queue_store.h"
#include "labbridge/core/logging.h"
#include "labbridge/core/version.h"

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view kComponent = "agent";

bool transient_startup_failure(
    const labbridge::agent::ControlPlaneClientError& error) {
    const auto status = error.http_status();
    return error.kind() == labbridge::agent::ControlPlaneErrorKind::Network ||
           error.kind() == labbridge::agent::ControlPlaneErrorKind::ServerError ||
           status == 408 || status == 429 ||
           (status >= 500 && status <= 599);
}
}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path =
        argc > 1 ? argv[1] : "deploy/env/agent.example.yaml";
    try {
        labbridge::core::log_info(kComponent, "starting LabBridge agent");
        const auto config = labbridge::agent::load_agent_config(config_path);
        labbridge::agent::AgentQueueStore queue_store{
            config.queue_db, config.node.node_code, config.max_pending_jobs,
            config.processed_fingerprint_capacity_per_task};
        labbridge::core::log_info(
            kComponent, "queue ready; pending_jobs=" +
                            std::to_string(queue_store.pending_job_count()));

        labbridge::agent::ControlPlaneClient control_client{
            config.server_url, config.request_timeout};
        labbridge::agent::PulledAgentConfig remote_config;
        bool connected = true;
        try {
            remote_config = labbridge::agent::perform_startup_handshake(
                control_client, config.node);
        } catch (const labbridge::agent::ControlPlaneClientError& error) {
            if (!transient_startup_failure(error)) {
                throw;
            }
            connected = false;
            labbridge::core::log_warn(
                kComponent,
                "control plane unavailable; starting in disconnected mode");
        }

        std::vector<labbridge::core::fs::path> allowed_local_roots;
        for (const auto& root : config.allowed_local_roots) {
            allowed_local_roots.emplace_back(root);
        }
        labbridge::agent::ReliableDeliveryClient delivery_client{
            control_client, queue_store, config.retry_initial, config.retry_max};
        labbridge::agent::TaskExecutor executor{
            delivery_client, queue_store, config.work_dir,
            std::move(allowed_local_roots),
            [] { return std::chrono::system_clock::now(); }};
        labbridge::agent::SystemSchedulerTimeSource scheduler_time;
        labbridge::agent::TaskScheduler scheduler{executor, scheduler_time};
        labbridge::agent::SystemRuntimeTimeSource runtime_time;
        labbridge::agent::AgentRuntime runtime{
            config.node, config.heartbeat_interval, config.config_poll_interval,
            control_client, std::move(remote_config), runtime_time, &scheduler,
            connected, config.retry_initial, config.retry_max};
        labbridge::agent::AgentApplication application{runtime, scheduler};
        labbridge::agent::ProcessSignalMonitor signal_monitor{[&application] {
            labbridge::core::log_info(kComponent, "stop requested");
            application.request_stop();
        }};
        static_cast<void>(application.run());
        labbridge::core::log_info(kComponent, "agent stopped normally");
        return 0;
    } catch (const std::exception& error) {
        labbridge::core::log_error(kComponent, error.what());
        return 1;
    }
}
