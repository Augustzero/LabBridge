#pragma once

#include "labbridge/server/application/agent_control_service.h"
#include "labbridge/server/http/http_json_response.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>

#include <functional>
#include <memory>
#include <string>

namespace labbridge::server {

class AgentControlHttpController
    : public std::enable_shared_from_this<AgentControlHttpController> {
public:
    using RegisterNodeHandler =
        std::function<labbridge::core::Status(const labbridge::core::NodeInfo&)>;
    using HeartbeatHandler = std::function<labbridge::core::Status(
        const labbridge::core::NodeHeartbeat&)>;
    using FindConfigHandler =
        std::function<AgentConfigResult(const std::string&)>;
    using ResponseCallback = http::ResponseCallback;

    AgentControlHttpController(RegisterNodeHandler register_node_handler,
                               HeartbeatHandler heartbeat_handler,
                               FindConfigHandler find_config_handler);

    void register_routes(drogon::HttpAppFramework& app);

    void post_register(const drogon::HttpRequestPtr& request,
                       ResponseCallback&& callback) const;
    void post_heartbeat(const drogon::HttpRequestPtr& request,
                        ResponseCallback&& callback) const;
    void get_config(const std::string& node_code,
                    ResponseCallback&& callback) const;

private:
    RegisterNodeHandler register_node_handler_;
    HeartbeatHandler heartbeat_handler_;
    FindConfigHandler find_config_handler_;
};

}  // namespace labbridge::server
