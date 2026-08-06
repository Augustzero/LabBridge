#include "labbridge/server/application/config_service.h"

#include <utility>

namespace labbridge::server {

ConfigService::ConfigService(INodeRepository& node_repository, IConfigRepository& config_repository)
    : node_repository_(node_repository), config_repository_(config_repository) {}

ConfigCreateResult ConfigService::create_data_source(const CreateDataSourceRequest& request) {
    if (request.node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"), {}};
    }
    if (request.name.empty()) {
        return {labbridge::core::Status::failure("data source name is required"), {}};
    }
    if (request.config_json.empty()) {
        return {labbridge::core::Status::failure("data source config_json is required"), {}};
    }
    if (!node_repository_.find_by_code(request.node_code).has_value()) {
        return {labbridge::core::Status::failure("node is not registered"), {}};
    }

    DataSourceRecord record;
    record.node_code = request.node_code;
    record.source_type = request.source_type;
    record.name = request.name;
    record.config_json = request.config_json;
    record.enabled = request.enabled;

    const auto id = config_repository_.create_data_source(std::move(record));
    return {labbridge::core::Status::success(), id};
}

ConfigCreateResult ConfigService::create_task(const CreateTaskRequest& request) {
    if (request.node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"), {}};
    }
    if (request.data_source_id.empty()) {
        return {labbridge::core::Status::failure("data_source_id is required"), {}};
    }
    if (request.name.empty()) {
        return {labbridge::core::Status::failure("task name is required"), {}};
    }
    if (request.task_type.empty()) {
        return {labbridge::core::Status::failure("task_type is required"), {}};
    }
    if (request.schedule_expr.empty()) {
        return {labbridge::core::Status::failure("schedule_expr is required"), {}};
    }
    if (request.parser_type.empty()) {
        return {labbridge::core::Status::failure("parser_type is required"), {}};
    }
    if (!node_repository_.find_by_code(request.node_code).has_value()) {
        return {labbridge::core::Status::failure("node is not registered"), {}};
    }

    const auto data_source = config_repository_.find_data_source(request.data_source_id);
    if (!data_source.has_value()) {
        return {labbridge::core::Status::failure("data source is not found"), {}};
    }
    // 任务必须绑定同节点的数据源，避免跨节点配置串用。
    if (data_source->node_code != request.node_code) {
        return {labbridge::core::Status::failure("data source does not belong to node"), {}};
    }

    TaskRecord record;
    record.node_code = request.node_code;
    record.data_source_id = request.data_source_id;
    record.name = request.name;
    record.task_type = request.task_type;
    record.schedule_expr = request.schedule_expr;
    record.parser_type = request.parser_type;
    record.qc_profile = request.qc_profile;
    record.enabled = request.enabled;

    const auto id = config_repository_.create_task(std::move(record));
    return {labbridge::core::Status::success(), id};
}

std::vector<TaskRecord> ConfigService::find_enabled_tasks(const std::string& node_code) const {
    if (node_code.empty()) {
        return {};
    }
    return config_repository_.find_enabled_tasks_by_node(node_code);
}

}  // namespace labbridge::server
