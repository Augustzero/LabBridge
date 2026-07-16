#include "labbridge/server/agent_report_service.h"

namespace labbridge::server {
namespace {

bool is_finish_status(labbridge::core::TaskRunStatus status) {
    return status == labbridge::core::TaskRunStatus::Succeeded ||
           status == labbridge::core::TaskRunStatus::Failed;
}

}  // namespace

AgentReportService::AgentReportService(TaskRunService& task_run_service,
                                       ResultService& result_service,
                                       QcService& qc_service,
                                       AlertService& alert_service)
    : task_run_service_(task_run_service),
      result_service_(result_service),
      qc_service_(qc_service),
      alert_service_(alert_service) {}

RawFileManifestResult AgentReportService::accept_raw_file_manifest(
    const RawFileManifestRequest& request) {
    const auto ownership_status = validate_task_run_node(request.task_run_id, request.node_code);
    if (!ownership_status.ok) {
        return {ownership_status, {}};
    }

    RawFileManifestResult result;
    result.status = labbridge::core::Status::success();
    for (const auto& file : request.files) {
        const auto created = result_service_.record_raw_file({
            request.task_run_id,
            request.node_code,
            file.original_name,
            file.file_hash,
            file.storage_path,
            file.size_bytes,
            file.source_mtime,
            file.ingest_status,
        });
        if (!created.status.ok) {
            result.status = created.status;
            return result;
        }
        result.raw_file_ids.push_back(created.id);
    }

    return result;
}

TaskRunReportResult AgentReportService::accept_task_run_report(
    const TaskRunReportRequest& request) {
    const auto ownership_status = validate_task_run_node(request.task_run_id, request.node_code);
    if (!ownership_status.ok) {
        return {ownership_status, {}, {}, {}};
    }
    if (!is_finish_status(request.status)) {
        return {labbridge::core::Status::failure("finish status must be succeeded or failed"),
                {},
                {},
                {}};
    }

    TaskRunReportResult result;
    result.status = labbridge::core::Status::success();
    for (const auto& parsed : request.parsed_records) {
        const auto parsed_record = result_service_.record_parsed_record({
            request.task_run_id,
            parsed.raw_file_id,
            parsed.record,
            parsed.parse_status,
        });
        if (!parsed_record.status.ok) {
            result.status = parsed_record.status;
            return result;
        }
        result.parsed_record_ids.push_back(parsed_record.id);

        for (const auto& qc : parsed.qc_results) {
            const auto qc_result = qc_service_.record_result({
                parsed_record.id,
                qc.qc_rule_id,
                qc.level,
                qc.result,
                qc.message,
            });
            if (!qc_result.status.ok) {
                result.status = qc_result.status;
                return result;
            }
            result.qc_result_ids.push_back(qc_result.id);

            const auto alert = alert_service_.create_from_qc_result_if_needed({qc_result.id});
            if (!alert.status.ok) {
                result.status = alert.status;
                return result;
            }
            if (!alert.id.empty()) {
                result.alert_ids.push_back(alert.id);
            }
        }
    }

    const auto finish_status = task_run_service_.finish({
        request.task_run_id,
        request.status,
        request.finished_at,
        request.items_total,
        request.items_success,
        request.items_failed,
        request.error_summary,
    });
    if (!finish_status.ok) {
        result.status = finish_status;
        return result;
    }

    return result;
}

labbridge::core::Status AgentReportService::validate_task_run_node(
    const std::string& task_run_id,
    const std::string& node_code) const {
    if (task_run_id.empty()) {
        return labbridge::core::Status::failure("task_run_id is required");
    }
    if (node_code.empty()) {
        return labbridge::core::Status::failure("node_code is required");
    }

    const auto task_run = task_run_service_.find_run(task_run_id);
    if (!task_run.has_value()) {
        return labbridge::core::Status::failure(labbridge::core::StatusCode::NotFound, "task run is not found");
    }
    if (task_run->node_code != node_code) {
        return labbridge::core::Status::failure(
            labbridge::core::StatusCode::Conflict, "task run does not belong to node");
    }
    return labbridge::core::Status::success();
}

}  // namespace labbridge::server
