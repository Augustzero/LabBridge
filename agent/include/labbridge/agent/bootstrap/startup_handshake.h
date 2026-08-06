#pragma once

#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/core/models.h"

namespace labbridge::agent {

PulledAgentConfig perform_startup_handshake(
    const ControlPlaneClient& client,
    const labbridge::core::NodeInfo& node);

}  // namespace labbridge::agent
