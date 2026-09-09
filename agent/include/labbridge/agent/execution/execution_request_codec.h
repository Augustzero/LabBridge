#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/core/models.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace labbridge::agent {

class ExecutionCodecError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string encode_task_config(const labbridge::core::TaskConfig& value);
labbridge::core::TaskConfig decode_task_config(const std::string& json);

// 返回 json 的编码变体供在线请求直接改写后发送；
// 字符串版本用于 SQLite 持久化，语义与 json 版本一致。
nlohmann::json encode_start_task_run_request_json(
    const StartTaskRunRequest& value);
std::string encode_start_task_run_request(const StartTaskRunRequest& value);
StartTaskRunRequest decode_start_task_run_request(const std::string& json);

nlohmann::json encode_raw_file_manifest_request_json(
    const RawFileManifestRequest& value);
std::string encode_raw_file_manifest_request(
    const RawFileManifestRequest& value);
RawFileManifestRequest decode_raw_file_manifest_request(
    const std::string& json);

nlohmann::json encode_task_run_report_request_json(
    const TaskRunReportRequest& value);
std::string encode_task_run_report_request(
    const TaskRunReportRequest& value);
TaskRunReportRequest decode_task_run_report_request(
    const std::string& json);

}  // namespace labbridge::agent