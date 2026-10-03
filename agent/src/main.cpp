#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/agent/bootstrap/process_signal_monitor.h"
#include "labbridge/agent/bootstrap/queue_command.h"
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
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view kComponent = "agent";
}  // namespace

int main(int argc, char* argv[]) {
    // queue 子命令是本地维护入口（list/show/retry），不进入运行时装配。
    // 子命令参数从 argv[2] 开始，argv[1] 的 "queue" 本身不参与解析。
    if (argc > 1 && std::string_view{argv[1]} == "queue") {
        return labbridge::agent::run_queue_command(argc - 2, argv + 2);
    }

    const std::string config_path =
        argc > 1 ? argv[1] : "deploy/env/agent.example.yaml";
    try {
        labbridge::core::log_info(kComponent, "starting LabBridge agent");
        const auto config = labbridge::agent::load_agent_config(config_path);
        labbridge::agent::AgentQueueStore queue_store{
            config.queue_db, config.node.node_code, config.max_pending_jobs,
            config.processed_fingerprint_capacity_per_task};
        // 队列就绪后立刻拿路径旁的 flock 并持有到进程退出：queue retry
        // 靠它发现 Agent 还在运行，误起第二个实例也会在这里被挡下。
        // 放在建库之后，首次部署时锁文件的目录还不存在。
        const labbridge::agent::AgentQueueLock queue_lock{config.queue_db};
        labbridge::core::log_info(
            kComponent, "queue ready; pending_jobs=" +
                            std::to_string(queue_store.pending_job_count()));

        labbridge::agent::ControlPlaneClient control_client{
            config.server_url, config.request_timeout,
            config.node.node_code, config.auth_token};
        labbridge::agent::PulledAgentConfig remote_config;
        bool connected = true;
        try {
            remote_config = labbridge::agent::perform_startup_handshake(
                control_client, config.node);
        } catch (const labbridge::agent::ControlPlaneClientError& error) {
            // 401/403 说明凭据配错，断网重连模式只会原地打转，直接退出等修正配置。
            if (error.is_auth_rejection() || !error.is_transient()) {
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
            control_client, queue_store, config.retry_initial,
            config.retry_max, config.max_request_body_bytes};
        labbridge::agent::TaskExecutor executor{
            delivery_client, queue_store, config.work_dir,
            std::move(allowed_local_roots), config.max_files_per_run,
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
