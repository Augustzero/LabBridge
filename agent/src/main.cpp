#include "labbridge/agent/bootstrap/agent_config.h"
#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/agent/bootstrap/process_signal_monitor.h"
#include "labbridge/agent/bootstrap/startup_handshake.h"
#include "labbridge/agent/runtime/agent_runtime.h"
#include "labbridge/core/logging.h"
#include "labbridge/core/version.h"

#include <exception>
#include <string>

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

        labbridge::agent::SystemRuntimeTimeSource time_source;
        labbridge::agent::AgentRuntime runtime{
            config.node,
            config.heartbeat_interval,
            config.config_poll_interval,
            client,
            remote_config,
            time_source};
        labbridge::agent::ProcessSignalMonitor signal_monitor{[&runtime] {
            labbridge::core::log_info(kComponent, "stop requested");
            runtime.request_stop();
        }};

        labbridge::core::log_info(kComponent, "runtime control loop started");
        const auto final_config = runtime.run();
        labbridge::core::log_info(
            kComponent,
            "runtime control loop stopped; enabled tasks: " +
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
