#include "labbridge/agent/startup_handshake.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace labbridge::agent {
namespace {

std::string current_utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto raw_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time{};
#if defined(_WIN32)
    gmtime_s(&utc_time, &raw_time);
#else
    gmtime_r(&raw_time, &utc_time);
#endif

    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

}  // namespace

PulledAgentConfig perform_startup_handshake(
    const ControlPlaneClient& client,
    const labbridge::core::NodeInfo& node) {
    client.register_node(node);
    client.send_heartbeat({
        node.node_code,
        node.agent_version,
        current_utc_timestamp(),
    });
    return client.fetch_config(node.node_code);
}

}  // namespace labbridge::agent
