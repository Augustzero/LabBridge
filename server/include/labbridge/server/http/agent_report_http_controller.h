#pragma once

#include "labbridge/server/application/agent_report_service.h"
#include "labbridge/server/http/http_json_response.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>

namespace labbridge::server {

class AgentReportHttpController
    : public std::enable_shared_from_this<AgentReportHttpController> {
public:
    using RawFileManifestHandler =
        std::function<RawFileManifestResult(const RawFileManifestRequest&)>;
    using TaskRunReportHandler =
        std::function<TaskRunReportResult(const TaskRunReportRequest&)>;
    using ResponseCallback = http::ResponseCallback;

    AgentReportHttpController(RawFileManifestHandler raw_file_manifest_handler,
                              TaskRunReportHandler task_run_report_handler);

    void register_routes(drogon::HttpAppFramework& app);

    void post_raw_file_manifest(const drogon::HttpRequestPtr& request,
                                ResponseCallback&& callback) const;
    void post_task_run_report(const drogon::HttpRequestPtr& request,
                              ResponseCallback&& callback) const;

private:
    RawFileManifestHandler raw_file_manifest_handler_;
    TaskRunReportHandler task_run_report_handler_;
};

}  // namespace labbridge::server
