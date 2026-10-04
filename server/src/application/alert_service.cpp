#include "labbridge/server/application/alert_service.h"

#include "labbridge/server/application/id_validation.h"

#include <optional>
#include <stdexcept>
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

AlertDispositionResult disposition_failure(labbridge::core::Status status) {
    return {std::move(status), std::nullopt};
}

// 锁定告警并按当前状态决定处置动作；调用方保证处于同一事务内。
AlertDispositionResult dispose_alert(
    const std::string& alert_id,
    IAlertRepository& alert_repository,
    bool acknowledge) {
    if (!is_positive_id(alert_id)) {
        return disposition_failure(labbridge::core::Status::failure(
            labbridge::core::StatusCode::InvalidArgument,
            "alert_id must be a positive integer"));
    }
    const auto alert = alert_repository.lock_by_id(alert_id);
    if (!alert.has_value()) {
        return disposition_failure(labbridge::core::Status::failure(
            labbridge::core::StatusCode::NotFound, "alert is not found"));
    }

    if (acknowledge) {
        if (alert->status == "closed") {
            return disposition_failure(labbridge::core::Status::failure(
                labbridge::core::StatusCode::Conflict,
                "alert is already closed"));
        }
        // 已确认的重复提交原样返回，不刷新首次确认时间。
        if (alert->status == "acknowledged") {
            return {labbridge::core::Status::success(), alert};
        }
    } else {
        // 已关闭的重复提交原样返回；open / acknowledged 都允许直接关闭。
        if (alert->status == "closed") {
            return {labbridge::core::Status::success(), alert};
        }
    }

    const auto updated = acknowledge
        ? alert_repository.save_acknowledged(alert_id)
        : alert_repository.save_closed(alert_id);
    if (!updated.has_value()) {
        // 锁定后行必然存在，写完却读不到说明 repository 契约被破坏。
        throw std::runtime_error("alert disposition update is not readable");
    }
    return {labbridge::core::Status::success(), *updated};
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
    return create_from_lookup(request, true);
}

AlertCreateResult AlertService::create_from_qc_result_if_needed(
    const CreateAlertFromQcResultRequest& request) {
    return create_from_lookup(request, false);
}

AlertCreateResult AlertService::create_from_lookup(
    const CreateAlertFromQcResultRequest& request,
    bool only_fail) {
    if (request.qc_result_id.empty()) {
        return {labbridge::core::Status::failure("qc_result_id is required"), {}};
    }

    const auto qc_result = qc_repository_.find_result(request.qc_result_id);
    if (!qc_result.has_value()) {
        return {labbridge::core::Status::failure(labbridge::core::StatusCode::NotFound, "qc result is not found"), {}};
    }
    if (!is_alert_qc_result(*qc_result)) {
        if (only_fail) {
            return {labbridge::core::Status::failure(
                        labbridge::core::StatusCode::Conflict,
                        "qc result does not require alert"),
                    {}};
        }
        return {labbridge::core::Status::success(), {}};
    }

    const auto parsed_record =
        result_repository_.find_parsed_record(qc_result->parsed_record_id);
    if (!parsed_record.has_value()) {
        return {labbridge::core::Status::failure(
                    labbridge::core::StatusCode::NotFound,
                    "parsed record is not found"),
                {}};
    }

    const auto task_run =
        task_run_repository_.find_by_id(parsed_record->task_run_id);
    if (!task_run.has_value()) {
        return {labbridge::core::Status::failure(labbridge::core::StatusCode::NotFound, "task run is not found"), {}};
    }

    return write_alert(*qc_result, parsed_record->task_run_id,
                       task_run->node_code);
}

AlertCreateResult AlertService::create_alert(
    const QcResultRecord& qc_result,
    const std::string& task_run_id,
    const std::string& node_code) {
    if (!is_alert_qc_result(qc_result)) {
        return {labbridge::core::Status::success(), {}};
    }
    return write_alert(qc_result, task_run_id, node_code);
}

AlertCreateResult AlertService::write_alert(
    const QcResultRecord& qc_result,
    const std::string& task_run_id,
    const std::string& node_code) {
    AlertRecord alert;
    alert.node_code = node_code;
    alert.task_run_id = task_run_id;
    alert.alert_type = "qc_result";
    alert.severity = alert_severity(qc_result);
    alert.message = alert_message(qc_result);
    alert.status = "open";

    const auto id = alert_repository_.create(std::move(alert));
    return {labbridge::core::Status::success(), id};
}

AlertDispositionResult AlertService::acknowledge_alert(
    const std::string& alert_id) {
    return dispose_alert(alert_id, alert_repository_, true);
}

AlertDispositionResult AlertService::close_alert(
    const std::string& alert_id) {
    return dispose_alert(alert_id, alert_repository_, false);
}

}  // namespace labbridge::server
