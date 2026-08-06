#pragma once

#include "labbridge/core/models.h"
#include "labbridge/core/result.h"
#include "labbridge/server/repositories/agent_report_receipt_repository.h"
#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/application/task_run_service.h"

#include <string>
#include <vector>

namespace labbridge::server {

struct RawFileManifestEntry {
    std::string original_name;
    std::string file_hash;
    std::string storage_path;
    long long size_bytes{0};
    std::string source_mtime;
    std::string ingest_status{"collected"};
};

struct RawFileManifestRequest {
    std::string task_run_id;
    std::string node_code;
    std::string idempotency_key;
    std::vector<RawFileManifestEntry> files;
};

struct RawFileManifestResult {
    labbridge::core::Status status;
    std::vector<std::string> raw_file_ids;
    bool replayed{false};
};

struct TaskRunReportQcResult {
    std::string qc_rule_id;
    std::string level;
    std::string result;
    std::string message;
};

struct TaskRunReportParsedRecord {
    std::string raw_file_id;
    labbridge::core::ParsedRecord record;
    std::string parse_status{"parsed"};
    std::vector<TaskRunReportQcResult> qc_results;
};

struct TaskRunReportRequest {
    std::string task_run_id;
    std::string node_code;
    std::string idempotency_key;
    labbridge::core::TaskRunStatus status{labbridge::core::TaskRunStatus::Succeeded};
    std::string finished_at;
    int items_total{0};
    int items_success{0};
    int items_failed{0};
    std::string error_summary;
    std::vector<TaskRunReportParsedRecord> parsed_records;
};

struct TaskRunReportResult {
    labbridge::core::Status status;
    std::vector<std::string> parsed_record_ids;
    std::vector<std::string> qc_result_ids;
    std::vector<std::string> alert_ids;
    bool replayed{false};
};

class AgentReportService {
public:
    AgentReportService(TaskRunService& task_run_service,
                       ResultService& result_service,
                       QcService& qc_service,
                       AlertService& alert_service,
                       IAgentReportReceiptRepository& receipt_repository);

    RawFileManifestResult accept_raw_file_manifest(
        const RawFileManifestRequest& request);
    TaskRunReportResult accept_task_run_report(const TaskRunReportRequest& request);

private:
    labbridge::core::Status validate_task_run_node(const std::string& task_run_id,
                                                   const std::string& node_code) const;

    TaskRunService& task_run_service_;
    ResultService& result_service_;
    QcService& qc_service_;
    AlertService& alert_service_;
    IAgentReportReceiptRepository& receipt_repository_;
};

}  // namespace labbridge::server
