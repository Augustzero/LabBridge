#include "labbridge/agent/bootstrap/startup_handshake.h"
#include "labbridge/agent/bootstrap/utc_time.h"

#include <chrono>

namespace labbridge::agent {

PulledAgentConfig perform_startup_handshake(
    const ControlPlaneClient& client,
    const labbridge::core::NodeInfo& node) {
    return perform_startup_handshake(
        client, node, std::chrono::system_clock::now());
}

PulledAgentConfig perform_startup_handshake(
    const ControlPlaneClient& client,
    const labbridge::core::NodeInfo& node,
    std::chrono::system_clock::time_point reported_at) {
    client.register_node(node);
    client.send_heartbeat({
        node.node_code,
        node.agent_version,
        format_utc_timestamp(reported_at),
    });
    return client.fetch_config(node.node_code);
}

}  // namespace labbridge::agent
