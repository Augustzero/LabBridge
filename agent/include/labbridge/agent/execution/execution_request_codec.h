#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/core/models.h"

#include <stdexcept>
#include <string>

namespace labbridge::agent {

class ExecutionCodecError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string encode_task_config(const labbridge::core::TaskConfig& value);
labbridge::core::TaskConfig decode_task_config(const std::string& json);

std::string encode_start_task_run_request(const StartTaskRunRequest& value);
StartTaskRunRequest decode_start_task_run_request(const std::string& json);

std::string encode_raw_file_manifest_request(
    const RawFileManifestRequest& value);
RawFileManifestRequest decode_raw_file_manifest_request(
    const std::string& json);

std::string encode_task_run_report_request(
    const TaskRunReportRequest& value);
TaskRunReportRequest decode_task_run_report_request(
    const std::string& json);

}  // namespace labbridge::agent