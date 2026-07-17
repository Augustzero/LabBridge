#include "labbridge/server/agent_report_receipt_repository.h"

#include <stdexcept>
#include <utility>

namespace labbridge::server {
namespace {

std::string request_type_key(AgentReportRequestType request_type) {
    return request_type == AgentReportRequestType::RawFileManifest ? "manifest" : "report";
}

void append_key_part(std::string& key, const std::string& value) {
    key += std::to_string(value.size());
    key.push_back(':');
    key += value;
}

std::string idempotency_key(const AgentReportReceiptClaim& request) {
    std::string key;
    append_key_part(key, request.node_code);
    append_key_part(key, request_type_key(request.request_type));
    append_key_part(key, request.idempotency_key);
    return key;
}

const AgentReportReceipt* find_completed_report_by_run(
    const std::unordered_map<std::string, AgentReportReceipt>& receipts,
    const std::string& task_run_id) {
    for (const auto& [key, receipt] : receipts) {
        (void)key;
        if (receipt.completed &&
            receipt.claim.request_type == AgentReportRequestType::TaskRunReport &&
            receipt.claim.task_run_id == task_run_id) {
            return &receipt;
        }
    }
    return nullptr;
}

}  // namespace

AgentReportReceiptClaimResult InMemoryAgentReportReceiptRepository::claim(
    AgentReportReceiptClaim request) {
    const auto key = idempotency_key(request);
    const auto existing = receipts_.find(key);
    if (existing != receipts_.end()) {
        if (existing->second.claim.request_fingerprint != request.request_fingerprint) {
            return {AgentReportReceiptClaimState::IdempotencyConflict, existing->second};
        }
        if (existing->second.completed) {
            return {AgentReportReceiptClaimState::Replay, existing->second};
        }
        return {AgentReportReceiptClaimState::Acquired, existing->second};
    }

    if (request.request_type == AgentReportRequestType::TaskRunReport) {
        const auto* completed = find_completed_report_by_run(receipts_, request.task_run_id);
        if (completed != nullptr) {
            return {AgentReportReceiptClaimState::TaskRunConflict, *completed};
        }
    }

    AgentReportReceipt receipt;
    receipt.id = std::to_string(next_receipt_id_++);
    receipt.claim = std::move(request);
    receipts_.emplace(key, receipt);
    return {AgentReportReceiptClaimState::Acquired, std::move(receipt)};
}

void InMemoryAgentReportReceiptRepository::complete(
    const std::string& receipt_id,
    AgentReportReceiptResponse response) {
    for (auto& [key, receipt] : receipts_) {
        (void)key;
        if (receipt.id != receipt_id) {
            continue;
        }
        if (receipt.completed) {
            throw std::runtime_error("agent report receipt is already completed");
        }
        receipt.response = std::move(response);
        receipt.completed = true;
        return;
    }
    throw std::runtime_error("agent report receipt is not found");
}

}  // namespace labbridge::server
