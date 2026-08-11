#include "labbridge/agent/bootstrap/agent_config.h"

#include "labbridge/agent/bootstrap/control_plane_client.h"
#include "labbridge/core/filesystem.h"
#include "labbridge/core/version.h"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace labbridge::agent {
namespace {

constexpr int kMaximumRequestTimeoutSeconds = 300;
constexpr int kMaximumIntervalSeconds = 86400;

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
                              const std::string& path,
                              int maximum) {
    const auto value = object[field];
    if (!value || !value.IsScalar()) {
        throw AgentConfigError(path + "." + field + " must be an integer");
    }

    try {
        const auto result = value.as<int>();
        if (result <= 0 || result > maximum) {
            throw AgentConfigError(
                path + "." + field + " must be between 1 and " +
                std::to_string(maximum));
        }
        return result;
    } catch (const AgentConfigError&) {
        throw;
    } catch (const YAML::Exception&) {
        throw AgentConfigError(path + "." + field + " must be an integer");
    }
}

labbridge::core::fs::path normalized_absolute_path(
    const labbridge::core::fs::path& value,
    const labbridge::core::fs::path& base_directory) {
    const auto absolute = value.is_absolute() ? value : base_directory / value;
    return labbridge::core::fs::weakly_canonical(absolute);
}

std::vector<std::string> required_allowed_roots(const YAML::Node& tasks) {
    const auto roots = tasks["allowed_local_roots"];
    if (!roots || !roots.IsSequence() || roots.size() == 0U) {
        throw AgentConfigError(
            "tasks.allowed_local_roots must be a non-empty sequence");
    }

    std::vector<std::string> result;
    result.reserve(roots.size());
    for (std::size_t index = 0; index < roots.size(); ++index) {
        const auto path = roots[index];
        if (!path.IsScalar()) {
            throw AgentConfigError(
                "tasks.allowed_local_roots entries must be absolute paths");
        }
        const auto text = path.as<std::string>();
        const labbridge::core::fs::path root_path{text};
        if (text.empty() || !root_path.is_absolute()) {
            throw AgentConfigError(
                "tasks.allowed_local_roots entries must be absolute paths");
        }
        result.push_back(
            labbridge::core::fs::weakly_canonical(root_path).string());
    }
    return result;
}

AgentStartupConfig parse_agent_config_node(
    const YAML::Node& root,
    const labbridge::core::fs::path& base_directory) {
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
        agent,
        "request_timeout_seconds",
        "agent",
        kMaximumRequestTimeoutSeconds);
    config.request_timeout = std::chrono::seconds{timeout_seconds};

    const auto heartbeat_interval_seconds = required_positive_integer(
        agent,
        "heartbeat_interval_seconds",
        "agent",
        kMaximumIntervalSeconds);
    config.heartbeat_interval =
        std::chrono::seconds{heartbeat_interval_seconds};

    const auto tasks = root["tasks"];
    if (!tasks || !tasks.IsMap()) {
        throw AgentConfigError("tasks configuration section is required");
    }
    const auto config_poll_interval_seconds = required_positive_integer(
        tasks,
        "poll_interval_seconds",
        "tasks",
        kMaximumIntervalSeconds);
    config.config_poll_interval =
        std::chrono::seconds{config_poll_interval_seconds};

    const auto storage = root["storage"];
    if (!storage || !storage.IsMap()) {
        throw AgentConfigError("storage configuration section is required");
    }
    config.work_dir = normalized_absolute_path(
                          required_string(storage, "work_dir", "storage"),
                          base_directory)
                          .string();
    config.allowed_local_roots = required_allowed_roots(tasks);
    return config;
}

}  // namespace

AgentStartupConfig parse_agent_config(std::string_view yaml_content) {
    try {
        return parse_agent_config_node(
            YAML::Load(std::string{yaml_content}),
            labbridge::core::fs::current_path());
    } catch (const AgentConfigError&) {
        throw;
    } catch (const YAML::Exception& error) {
        throw AgentConfigError(
            "failed to parse agent configuration: " + std::string{error.what()});
    } catch (const std::invalid_argument& error) {
        throw AgentConfigError(
            "invalid agent.server_url: " + std::string{error.what()});
    } catch (const labbridge::core::fs::filesystem_error& error) {
        throw AgentConfigError(
            "invalid local path configuration: " + std::string{error.what()});
    }
}

AgentStartupConfig load_agent_config(const std::string& path) {
    try {
        const auto absolute_path = labbridge::core::fs::absolute(path);
        return parse_agent_config_node(
            YAML::LoadFile(path), absolute_path.parent_path());
    } catch (const AgentConfigError&) {
        throw;
    } catch (const YAML::Exception& error) {
        throw AgentConfigError(
            "failed to load agent configuration '" + path + "': " + error.what());
    } catch (const std::invalid_argument& error) {
        throw AgentConfigError(
            "invalid agent.server_url: " + std::string{error.what()});
    } catch (const labbridge::core::fs::filesystem_error& error) {
        throw AgentConfigError(
            "invalid local path configuration: " + std::string{error.what()});
    }
}

}  // namespace labbridge::agent
