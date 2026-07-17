#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace labbridge::server {

enum class AgentReportRequestType {
    RawFileManifest,
    TaskRunReport,
};

struct AgentReportReceiptClaim {
    std::string task_run_id;
    std::string node_code;
    AgentReportRequestType request_type{AgentReportRequestType::RawFileManifest};
    std::string idempotency_key;
    std::string request_fingerprint;
};

struct AgentReportReceiptResponse {
    std::vector<std::string> raw_file_ids;
    std::vector<std::string> parsed_record_ids;
    std::vector<std::string> qc_result_ids;
    std::vector<std::string> alert_ids;
};

struct AgentReportReceipt {
    std::string id;
    AgentReportReceiptClaim claim;
    AgentReportReceiptResponse response;
    bool completed{false};
};

enum class AgentReportReceiptClaimState {
    Acquired,
    Replay,
    IdempotencyConflict,
    TaskRunConflict,
};

struct AgentReportReceiptClaimResult {
    AgentReportReceiptClaimState state{AgentReportReceiptClaimState::Acquired};
    AgentReportReceipt receipt;
};

class IAgentReportReceiptRepository {
public:
    virtual ~IAgentReportReceiptRepository() = default;

    virtual AgentReportReceiptClaimResult claim(AgentReportReceiptClaim request) = 0;
    virtual void complete(const std::string& receipt_id,
                          AgentReportReceiptResponse response) = 0;
};

class InMemoryAgentReportReceiptRepository final
    : public IAgentReportReceiptRepository {
public:
    AgentReportReceiptClaimResult claim(AgentReportReceiptClaim request) override;
    void complete(const std::string& receipt_id,
                  AgentReportReceiptResponse response) override;

private:
    int next_receipt_id_{1};
    std::unordered_map<std::string, AgentReportReceipt> receipts_;
};

}  // namespace labbridge::server
