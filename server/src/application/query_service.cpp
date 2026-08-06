#include "labbridge/server/application/query_service.h"

namespace labbridge::server {

ControlPlaneQueryService::ControlPlaneQueryService(INodeRepository& node_repository,
                                                   IConfigRepository& config_repository,
                                                   ITaskRunRepository& task_run_repository,
                                                   IResultRepository& result_repository,
                                                   IQcRepository& qc_repository,
                                                   IAlertRepository& alert_repository)
    : node_repository_(node_repository),
      config_repository_(config_repository),
      task_run_repository_(task_run_repository),
      result_repository_(result_repository),
      qc_repository_(qc_repository),
      alert_repository_(alert_repository) {}

NodeOverviewQueryResult ControlPlaneQueryService::find_node_overview(
    const std::string& node_code) const {
    if (node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"), std::nullopt, {}, {}};
    }

    auto node = node_repository_.find_by_code(node_code);
    if (!node.has_value()) {
        return {labbridge::core::Status::failure("node is not found"), std::nullopt, {}, {}};
    }

    NodeOverviewQueryResult result;
    result.status = labbridge::core::Status::success();
    result.node = node;
    result.enabled_tasks = config_repository_.find_enabled_tasks_by_node(node_code);
    result.alerts = alert_repository_.find_by_node(node_code);
    return result;
}

TaskRunDetailQueryResult ControlPlaneQueryService::find_task_run_detail(
    const std::string& node_code,
    const std::string& task_run_id) const {
    if (node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"),
                std::nullopt,
                {},
                {},
                {},
                {}};
    }
    if (task_run_id.empty()) {
        return {labbridge::core::Status::failure("task_run_id is required"),
                std::nullopt,
                {},
                {},
                {},
                {}};
    }

    if (!node_repository_.find_by_code(node_code).has_value()) {
        return {labbridge::core::Status::failure("node is not found"), std::nullopt, {}, {}, {}, {}};
    }

    auto task_run = task_run_repository_.find_by_id(task_run_id);
    if (!task_run.has_value()) {
        return {labbridge::core::Status::failure("task run is not found"),
                std::nullopt,
                {},
                {},
                {},
                {}};
    }
    if (task_run->node_code != node_code) {
        return {labbridge::core::Status::failure("task run does not belong to node"),
                std::nullopt,
                {},
                {},
                {},
                {}};
    }

    TaskRunDetailQueryResult result;
    result.status = labbridge::core::Status::success();
    result.task_run = task_run;
    result.raw_files = result_repository_.find_raw_files_by_run(task_run_id);
    result.parsed_records = result_repository_.find_parsed_records_by_run(task_run_id);
    for (const auto& parsed_record : result.parsed_records) {
        auto qc_results = qc_repository_.find_results_by_parsed_record(parsed_record.id);
        result.qc_results.insert(result.qc_results.end(), qc_results.begin(), qc_results.end());
    }
    result.alerts = alert_repository_.find_by_task_run(task_run_id);
    return result;
}

}  // namespace labbridge::server
