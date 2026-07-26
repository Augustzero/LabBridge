#include "labbridge/agent/agent_config.h"

#include "labbridge/agent/control_plane_client.h"
#include "labbridge/core/version.h"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>

namespace labbridge::agent {
namespace {

constexpr int kMaximumTimeoutSeconds = 300;

bool is_blank(const std::string& value) {
    for (const unsigned char ch : value) {
        if (!std::isspace(ch)) {
            return false;
        }
    }
    return true;
}

std::string required_string(const YAML::Node& object,
                            const std::string& field,
                            const std::string& path) {
    const auto value = object[field];
    if (!value || !value.IsScalar()) {
        throw AgentConfigError(path + "." + field + " must be a string");
    }

    try {
        auto text = value.as<std::string>();
        if (text.empty() || is_blank(text)) {
            throw AgentConfigError(path + "." + field + " must not be empty");
        }
        return text;
    } catch (const YAML::Exception&) {
        throw AgentConfigError(path + "." + field + " must be a string");
    }
}

int required_positive_integer(const YAML::Node& object,
                              const std::string& field,
                              const std::string& path) {
    const auto value = object[field];
    if (!value || !value.IsScalar()) {
        throw AgentConfigError(path + "." + field + " must be an integer");
    }

    try {
        const auto result = value.as<int>();
        if (result <= 0 || result > kMaximumTimeoutSeconds) {
            throw AgentConfigError(
                path + "." + field + " must be between 1 and " +
                std::to_string(kMaximumTimeoutSeconds));
        }
        return result;
    } catch (const AgentConfigError&) {
        throw;
    } catch (const YAML::Exception&) {
        throw AgentConfigError(path + "." + field + " must be an integer");
    }
}

}  // namespace

AgentStartupConfig load_agent_config(const std::string& path) {
    try {
        const auto root = YAML::LoadFile(path);
        const auto agent = root["agent"];
        if (!agent || !agent.IsMap()) {
            throw AgentConfigError("agent configuration section is required");
        }

        AgentStartupConfig config;
        config.node.node_code = required_string(agent, "node_code", "agent");
        config.node.name = required_string(agent, "name", "agent");
        config.node.agent_version = labbridge::core::kVersion;
        config.server_url = required_string(agent, "server_url", "agent");
        validate_control_plane_url(config.server_url);

        const auto timeout_seconds = required_positive_integer(
            agent, "request_timeout_seconds", "agent");
        config.request_timeout = std::chrono::seconds{timeout_seconds};
        return config;
    } catch (const AgentConfigError&) {
        throw;
    } catch (const YAML::Exception& error) {
        throw AgentConfigError(
            "failed to load agent configuration '" + path + "': " + error.what());
    } catch (const std::invalid_argument& error) {
        throw AgentConfigError(
            "invalid agent.server_url: " + std::string{error.what()});
    }
}

}  // namespace labbridge::agent
