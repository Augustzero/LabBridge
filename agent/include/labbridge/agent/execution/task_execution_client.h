#pragma once

#include "labbridge/core/models.h"

#include <string>
#include <vector>

namespace labbridge::agent {

struct StartTaskRunRequest {
    std::string node_code;
    std::string task_id;
    std::string execution_key;
    std::string scheduled_for;
    std::string started_at;
    std::string trigger_type{"scheduled"};
};

struct StartTaskRunResult {
    std::string task_run_id;
    bool replayed{false};
};

struct RawFileManifestEntry {
    std::string original_name;
    std::string file_hash;
    std::string storage_path;
    long long size_bytes{0};
    std::string source_mtime;
    std::string ingest_status{"archived_local"};
};

struct RawFileManifestRequest {
    std::string task_run_id;
    std::string node_code;
    std::string idempotency_key;
    std::vector<RawFileManifestEntry> files;
};

struct RawFileManifestResult {
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
    labbridge::core::TaskRunStatus status{
        labbridge::core::TaskRunStatus::Succeeded};
    std::string finished_at;
    int items_total{0};
    int items_success{0};
    int items_failed{0};
    std::string error_summary;
    std::vector<TaskRunReportParsedRecord> parsed_records;
};

struct TaskRunReportResult {
    std::vector<std::string> parsed_record_ids;
    std::vector<std::string> qc_result_ids;
    std::vector<std::string> alert_ids;
    bool replayed{false};
};

std::string make_scheduled_execution_key(
    const std::string& node_code,
    const std::string& task_id,
    const std::string& scheduled_for);
std::string make_manifest_idempotency_key(
    const std::string& node_code,
    const std::string& task_run_id);
std::string make_report_idempotency_key(
    const std::string& node_code,
    const std::string& task_run_id);

class ITaskExecutionClient {
public:
    virtual ~ITaskExecutionClient() = default;

    virtual StartTaskRunResult start_task_run(
        const StartTaskRunRequest& request) const = 0;
    virtual RawFileManifestResult report_raw_file_manifest(
        const RawFileManifestRequest& request) const = 0;
    virtual TaskRunReportResult report_task_run(
        const TaskRunReportRequest& request) const = 0;
};

}  // namespace labbridge::agent
