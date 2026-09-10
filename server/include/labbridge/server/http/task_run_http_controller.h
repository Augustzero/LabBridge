#pragma once

#include "labbridge/server/application/task_run_service.h"
#include "labbridge/server/http/http_authenticator.h"
#include "labbridge/server/http/http_json_response.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>

namespace labbridge::server {

class TaskRunHttpController
    : public std::enable_shared_from_this<TaskRunHttpController> {
public:
    using StartHandler =
        std::function<TaskRunCreateResult(const StartTaskRunRequest&)>;
    using ResponseCallback = http::ResponseCallback;

    TaskRunHttpController(std::shared_ptr<const HttpAuthenticator> authenticator,
                          StartHandler start_handler);

    void register_routes(drogon::HttpAppFramework& app);
    void post_start(const drogon::HttpRequestPtr& request,
                    ResponseCallback&& callback) const;

private:
    std::shared_ptr<const HttpAuthenticator> authenticator_;
    StartHandler start_handler_;
};

}  // namespace labbridge::server
