#pragma once

#include "labbridge/server/application/agent_control_service.h"
#include "labbridge/server/http/http_authenticator.h"
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

    AgentControlHttpController(
        std::shared_ptr<const HttpAuthenticator> authenticator,
        RegisterNodeHandler register_node_handler,
        HeartbeatHandler heartbeat_handler,
        FindConfigHandler find_config_handler);

    void register_routes(drogon::HttpAppFramework& app);

    void post_register(const drogon::HttpRequestPtr& request,
                       ResponseCallback&& callback) const;
    void post_heartbeat(const drogon::HttpRequestPtr& request,
                        ResponseCallback&& callback) const;
    void get_config(const drogon::HttpRequestPtr& request,
                    const std::string& node_code,
                    ResponseCallback&& callback) const;

private:
    std::shared_ptr<const HttpAuthenticator> authenticator_;
    RegisterNodeHandler register_node_handler_;
    HeartbeatHandler heartbeat_handler_;
    FindConfigHandler find_config_handler_;
};

}  // namespace labbridge::server
