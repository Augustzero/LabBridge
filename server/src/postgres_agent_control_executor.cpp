#include "labbridge/server/postgres_agent_control_executor.h"

#include "labbridge/server/libpq_sql_session.h"
#include "labbridge/server/postgres_config_repository.h"
#include "labbridge/server/postgres_node_repository.h"

#include <utility>

namespace labbridge::server {
namespace {

class AgentControlRequestScope {
public:
    explicit AgentControlRequestScope(const std::string& connection_info)
        : session_(connection_info),
          node_repository_(session_),
          config_repository_(session_),
          node_service_(node_repository_),
          config_service_(node_repository_, config_repository_),
          agent_control_service_(node_service_, config_service_) {}

    AgentControlService& agent_control_service() {
        return agent_control_service_;
    }

private:
    LibpqSqlSession session_;
    PostgresNodeRepository node_repository_;
    PostgresConfigRepository config_repository_;
    NodeService node_service_;
    ConfigService config_service_;
    AgentControlService agent_control_service_;
};

}  // namespace

PostgresAgentControlExecutor::PostgresAgentControlExecutor(
    std::string connection_info)
    : connection_info_(std::move(connection_info)) {}

labbridge::core::Status PostgresAgentControlExecutor::register_node(
    const labbridge::core::NodeInfo& node) const {
    AgentControlRequestScope scope{connection_info_};
    return scope.agent_control_service().register_node(node);
}

labbridge::core::Status PostgresAgentControlExecutor::accept_heartbeat(
    const labbridge::core::NodeHeartbeat& heartbeat) const {
    AgentControlRequestScope scope{connection_info_};
    return scope.agent_control_service().accept_heartbeat(heartbeat);
}

AgentConfigResult PostgresAgentControlExecutor::find_config(
    const std::string& node_code) const {
    AgentControlRequestScope scope{connection_info_};
    return scope.agent_control_service().find_config(node_code);
}

}  // namespace labbridge::server
