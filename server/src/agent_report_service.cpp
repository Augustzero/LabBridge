#include "labbridge/server/agent_report_service.h"

#include <openssl/evp.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace labbridge::server {
namespace {

bool is_finish_status(labbridge::core::TaskRunStatus status) {
    return status == labbridge::core::TaskRunStatus::Succeeded ||
           status == labbridge::core::TaskRunStatus::Failed;
}

bool is_terminal_status(labbridge::core::TaskRunStatus status) {
    return status == labbridge::core::TaskRunStatus::Succeeded ||
           status == labbridge::core::TaskRunStatus::Failed;
}

labbridge::core::Status validate_idempotency_key(const std::string& key) {
    if (key.empty()) {
        return labbridge::core::Status::failure("idempotency_key is required");
    }
    if (key.size() > 128) {
        return labbridge::core::Status::failure(
            "idempotency_key must not exceed 128 characters");
    }
    return labbridge::core::Status::success();
}

class FingerprintBuilder {
public:
    void append(std::string_view value) {
        canonical_ += std::to_string(value.size());
        canonical_.push_back(':');
        canonical_.append(value);
    }

    void append(long long value) {
        append(std::to_string(value));
    }

    std::string finish() const {
        using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
        Context context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
        if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(context.get(), canonical_.data(), canonical_.size()) != 1) {
            throw std::runtime_error("failed to initialize agent report fingerprint");
        }

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_size = 0;
        if (EVP_DigestFinal_ex(context.get(), digest, &digest_size) != 1) {
            throw std::runtime_error("failed to create agent report fingerprint");
        }

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (unsigned int index = 0; index < digest_size; ++index) {
            output << std::setw(2) << static_cast<unsigned int>(digest[index]);
        }
        return output.str();
    }

private:
    std::string canonical_;
};

std::string task_run_status_value(labbridge::core::TaskRunStatus status) {
    switch (status) {
        case labbridge::core::TaskRunStatus::Pending:
            return "pending";
        case labbridge::core::TaskRunStatus::Running:
            return "running";
        case labbridge::core::TaskRunStatus::Succeeded:
            return "succeeded";
        case labbridge::core::TaskRunStatus::Failed:
            return "failed";
    }
    throw std::runtime_error("unsupported task run status");
}

std::string manifest_fingerprint(const RawFileManifestRequest& request) {
    FingerprintBuilder builder;
    builder.append("raw_file_manifest");
    builder.append(request.task_run_id);
    builder.append(request.node_code);
    builder.append(static_cast<long long>(request.files.size()));
    for (const auto& file : request.files) {
        builder.append(file.original_name);
        builder.append(file.file_hash);
        builder.append(file.storage_path);
        builder.append(file.size_bytes);
        builder.append(file.source_mtime);
        builder.append(file.ingest_status.empty() ? "collected" : file.ingest_status);
    }
    return builder.finish();
}

std::string report_fingerprint(const TaskRunReportRequest& request) {
    FingerprintBuilder builder;
    builder.append("task_run_report");
    builder.append(request.task_run_id);
    builder.append(request.node_code);
    builder.append(task_run_status_value(request.status));
    builder.append(request.finished_at);
    builder.append(request.items_total);
    builder.append(request.items_success);
    builder.append(request.items_failed);
    builder.append(request.error_summary);
    builder.append(static_cast<long long>(request.parsed_records.size()));
    for (const auto& parsed : request.parsed_records) {
        builder.append(parsed.raw_file_id);
        builder.append(parsed.record.station_code);
        builder.append(parsed.record.device_code);
        builder.append(parsed.record.record_time);
        builder.append(parsed.record.payload_json);
        builder.append(parsed.parse_status.empty() ? "parsed" : parsed.parse_status);
        builder.append(static_cast<long long>(parsed.qc_results.size()));
        for (const auto& qc : parsed.qc_results) {
            builder.append(qc.qc_rule_id);
            builder.append(qc.level);
            builder.append(qc.result);
            builder.append(qc.message);
        }
    }
    return builder.finish();
}

labbridge::core::Status claim_error(AgentReportReceiptClaimState state) {
    if (state == AgentReportReceiptClaimState::IdempotencyConflict) {
        return labbridge::core::Status::failure(
            labbridge::core::StatusCode::Conflict,
            "idempotency_key was already used for a different request");
    }
    return labbridge::core::Status::failure(
        labbridge::core::StatusCode::Conflict,
        "task run already has a completed report");
}

}  // namespace

AgentReportService::AgentReportService(
    TaskRunService& task_run_service,
    ResultService& result_service,
    QcService& qc_service,
    AlertService& alert_service,
    IAgentReportReceiptRepository& receipt_repository)
    : task_run_service_(task_run_service),
      result_service_(result_service),
      qc_service_(qc_service),
      alert_service_(alert_service),
      receipt_repository_(receipt_repository) {}

RawFileManifestResult AgentReportService::accept_raw_file_manifest(
    const RawFileManifestRequest& request) {
    const auto idempotency_status = validate_idempotency_key(request.idempotency_key);
    if (!idempotency_status.ok) {
        return {idempotency_status, {}};
    }

    const auto ownership_status = validate_task_run_node(request.task_run_id, request.node_code);
    if (!ownership_status.ok) {
        return {ownership_status, {}};
    }

    const auto receipt = receipt_repository_.claim({
        request.task_run_id,
        request.node_code,
        AgentReportRequestType::RawFileManifest,
        request.idempotency_key,
        manifest_fingerprint(request),
    });
    if (receipt.state == AgentReportReceiptClaimState::Replay) {
        return {
            labbridge::core::Status::success(),
            receipt.receipt.response.raw_file_ids,
            true,
        };
    }
    if (receipt.state != AgentReportReceiptClaimState::Acquired) {
        return {claim_error(receipt.state), {}};
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

    AgentReportReceiptResponse response;
    response.raw_file_ids = result.raw_file_ids;
    receipt_repository_.complete(receipt.receipt.id, std::move(response));
    return result;
}

TaskRunReportResult AgentReportService::accept_task_run_report(
    const TaskRunReportRequest& request) {
    const auto idempotency_status = validate_idempotency_key(request.idempotency_key);
    if (!idempotency_status.ok) {
        return {idempotency_status, {}, {}, {}};
    }

    const auto ownership_status = validate_task_run_node(request.task_run_id, request.node_code);
    if (!ownership_status.ok) {
        return {ownership_status, {}, {}, {}};
    }
    if (!is_finish_status(request.status)) {
        return {
            labbridge::core::Status::failure(
                "finish status must be succeeded or failed"),
            {},
            {},
            {},
        };
    }

    const auto receipt = receipt_repository_.claim({
        request.task_run_id,
        request.node_code,
        AgentReportRequestType::TaskRunReport,
        request.idempotency_key,
        report_fingerprint(request),
    });
    if (receipt.state == AgentReportReceiptClaimState::Replay) {
        return {
            labbridge::core::Status::success(),
            receipt.receipt.response.parsed_record_ids,
            receipt.receipt.response.qc_result_ids,
            receipt.receipt.response.alert_ids,
            true,
        };
    }
    if (receipt.state != AgentReportReceiptClaimState::Acquired) {
        return {claim_error(receipt.state), {}, {}, {}};
    }

    const auto task_run = task_run_service_.find_run(request.task_run_id);
    if (!task_run.has_value()) {
        return {
            labbridge::core::Status::failure(
                labbridge::core::StatusCode::NotFound,
                "task run is not found"),
            {},
            {},
            {},
        };
    }
    if (is_terminal_status(task_run->status)) {
        return {
            labbridge::core::Status::failure(
                labbridge::core::StatusCode::Conflict,
                "task run is already finished"),
            {},
            {},
            {},
        };
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

            const auto alert =
                alert_service_.create_from_qc_result_if_needed({qc_result.id});
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

    AgentReportReceiptResponse response;
    response.parsed_record_ids = result.parsed_record_ids;
    response.qc_result_ids = result.qc_result_ids;
    response.alert_ids = result.alert_ids;
    receipt_repository_.complete(receipt.receipt.id, std::move(response));
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
        return labbridge::core::Status::failure(
            labbridge::core::StatusCode::NotFound,
            "task run is not found");
    }
    if (task_run->node_code != node_code) {
        return labbridge::core::Status::failure(
            labbridge::core::StatusCode::Conflict,
            "task run does not belong to node");
    }
    return labbridge::core::Status::success();
}

}  // namespace labbridge::server
