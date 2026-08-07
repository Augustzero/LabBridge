#pragma once

#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/core/models.h"

#include <chrono>

namespace labbridge::agent {

PulledAgentConfig perform_startup_handshake(
    const ControlPlaneClient& client,
    const labbridge::core::NodeInfo& node);
PulledAgentConfig perform_startup_handshake(
    const ControlPlaneClient& client,
    const labbridge::core::NodeInfo& node,
    std::chrono::system_clock::time_point reported_at);

}  // namespace labbridge::agent
