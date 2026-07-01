#include "labbridge/server/alert_service.h"

#include <utility>

namespace labbridge::server {
namespace {

bool is_alert_qc_result(const QcResultRecord& result) {
    return result.level == "warning" || result.level == "failed" || result.result == "warning" ||
           result.result == "failed";
}

std::string alert_severity(const QcResultRecord& result) {
    if (result.level == "warning" || result.level == "failed") {
        return result.level;
    }
    return result.result;
}

std::string alert_message(const QcResultRecord& result) {
    if (!result.message.empty()) {
        return result.message;
    }
    return "qc result " + result.id + " is " + result.result;
}

}  // namespace

AlertService::AlertService(ITaskRunRepository& task_run_repository,
                           IResultRepository& result_repository,
                           IQcRepository& qc_repository,
                           IAlertRepository& alert_repository)
    : task_run_repository_(task_run_repository),
      result_repository_(result_repository),
      qc_repository_(qc_repository),
      alert_repository_(alert_repository) {}

AlertCreateResult AlertService::create_from_qc_result(
    const CreateAlertFromQcResultRequest& request) {
    if (request.qc_result_id.empty()) {
        return {labbridge::core::Status::failure("qc_result_id is required"), {}};
    }

    const auto qc_result = qc_repository_.find_result(request.qc_result_id);
    if (!qc_result.has_value()) {
        return {labbridge::core::Status::failure("qc result is not found"), {}};
    }
    if (!is_alert_qc_result(*qc_result)) {
        return {labbridge::core::Status::failure("qc result does not require alert"), {}};
    }

    const auto parsed_record = result_repository_.find_parsed_record(qc_result->parsed_record_id);
    if (!parsed_record.has_value()) {
        return {labbridge::core::Status::failure("parsed record is not found"), {}};
    }

    const auto task_run = task_run_repository_.find_by_id(parsed_record->task_run_id);
    if (!task_run.has_value()) {
        return {labbridge::core::Status::failure("task run is not found"), {}};
    }

    AlertRecord alert;
    alert.node_code = task_run->node_code;
    alert.task_run_id = parsed_record->task_run_id;
    alert.alert_type = "qc_result";
    alert.severity = alert_severity(*qc_result);
    alert.message = alert_message(*qc_result);
    alert.status = "open";

    const auto id = alert_repository_.create(std::move(alert));
    return {labbridge::core::Status::success(), id};
}

AlertCreateResult AlertService::create_from_qc_result_if_needed(
    const CreateAlertFromQcResultRequest& request) {
    if (request.qc_result_id.empty()) {
        return {labbridge::core::Status::failure("qc_result_id is required"), {}};
    }

    const auto qc_result = qc_repository_.find_result(request.qc_result_id);
    if (!qc_result.has_value()) {
        return {labbridge::core::Status::failure("qc result is not found"), {}};
    }
    if (!is_alert_qc_result(*qc_result)) {
        return {labbridge::core::Status::success(), {}};
    }

    return create_from_qc_result(request);
}

std::vector<AlertRecord> AlertService::find_alerts_by_node(
    const std::string& node_code) const {
    if (node_code.empty()) {
        return {};
    }
    return alert_repository_.find_by_node(node_code);
}

std::vector<AlertRecord> AlertService::find_alerts_by_task_run(
    const std::string& task_run_id) const {
    if (task_run_id.empty()) {
        return {};
    }
    return alert_repository_.find_by_task_run(task_run_id);
}

}  // namespace labbridge::server
