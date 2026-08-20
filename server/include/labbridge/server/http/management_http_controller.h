#pragma once

#include "labbridge/server/application/management_query_service.h"
#include "labbridge/server/http/http_json_response.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>

#include <functional>
#include <memory>
#include <string>

namespace labbridge::server {

struct ManagementQueryHandlers {
    std::function<ManagementPageResult<ManagementNode>(const NodeListRequest&)>
        list_nodes;
    std::function<ManagementItemResult<ManagementNodeSummary>(const std::string&)>
        find_node;
    std::function<ManagementPageResult<DataSourceRecord>(
        const NodeScopedListRequest&)> list_data_sources;
    std::function<ManagementPageResult<QcRuleRecord>(const QcRuleListRequest&)>
        list_qc_rules;
    std::function<ManagementPageResult<TaskRecord>(const NodeScopedListRequest&)>
        list_tasks;
    std::function<ManagementPageResult<ManagementTaskRun>(
        const TaskRunListRequest&)> list_task_runs;
    std::function<ManagementItemResult<ManagementTaskRunSummary>(
        const std::string&, const std::string&)> find_task_run;
    std::function<ManagementPageResult<RawFileRecord>(
        const RunScopedListRequest&)> list_raw_files;
    std::function<ManagementPageResult<ParsedRecordRecord>(
        const RunScopedListRequest&)> list_parsed_records;
    std::function<ManagementPageResult<QcResultRecord>(
        const QcResultListRequest&)> list_qc_results;
    std::function<ManagementPageResult<AlertRecord>(const AlertListRequest&)>
        list_alerts;
};

class ManagementHttpController
    : public std::enable_shared_from_this<ManagementHttpController> {
public:
    using ResponseCallback = http::ResponseCallback;

    explicit ManagementHttpController(ManagementQueryHandlers handlers);

    void register_routes(drogon::HttpAppFramework& app);

    void get_nodes(const drogon::HttpRequestPtr& request,
                   ResponseCallback&& callback) const;
    void get_node(const drogon::HttpRequestPtr& request,
                  const std::string& node_code,
                  ResponseCallback&& callback) const;
    void get_data_sources(const drogon::HttpRequestPtr& request,
                          ResponseCallback&& callback) const;
    void get_qc_rules(const drogon::HttpRequestPtr& request,
                      ResponseCallback&& callback) const;
    void get_tasks(const drogon::HttpRequestPtr& request,
                   ResponseCallback&& callback) const;
    void get_task_runs(const drogon::HttpRequestPtr& request,
                       ResponseCallback&& callback) const;
    void get_task_run(const drogon::HttpRequestPtr& request,
                      const std::string& task_run_id,
                      ResponseCallback&& callback) const;
    void get_raw_files(const drogon::HttpRequestPtr& request,
                       ResponseCallback&& callback) const;
    void get_parsed_records(const drogon::HttpRequestPtr& request,
                            ResponseCallback&& callback) const;
    void get_qc_results(const drogon::HttpRequestPtr& request,
                        ResponseCallback&& callback) const;
    void get_alerts(const drogon::HttpRequestPtr& request,
                    ResponseCallback&& callback) const;

private:
    ManagementQueryHandlers handlers_;
};

}  // namespace labbridge::server
