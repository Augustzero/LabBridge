#include "labbridge/server/result_service.h"

#include <utility>

namespace labbridge::server {

ResultService::ResultService(ITaskRunRepository& task_run_repository,
                             IResultRepository& result_repository)
    : task_run_repository_(task_run_repository), result_repository_(result_repository) {}

ResultCreateResult ResultService::record_raw_file(const RecordRawFileRequest& request) {
    if (request.task_run_id.empty()) {
        return {labbridge::core::Status::failure("task_run_id is required"), {}};
    }
    if (request.node_code.empty()) {
        return {labbridge::core::Status::failure("node_code is required"), {}};
    }
    if (request.original_name.empty()) {
        return {labbridge::core::Status::failure("original_name is required"), {}};
    }
    if (request.storage_path.empty()) {
        return {labbridge::core::Status::failure("storage_path is required"), {}};
    }

    const auto task_run = task_run_repository_.find_by_id(request.task_run_id);
    if (!task_run.has_value()) {
        return {labbridge::core::Status::failure("task run is not found"), {}};
    }
    if (task_run->node_code != request.node_code) {
        return {labbridge::core::Status::failure("task run does not belong to node"), {}};
    }

    RawFileRecord record;
    record.task_run_id = request.task_run_id;
    record.node_code = request.node_code;
    record.original_name = request.original_name;
    record.file_hash = request.file_hash;
    record.storage_path = request.storage_path;
    record.size_bytes = request.size_bytes;
    record.source_mtime = request.source_mtime;
    record.ingest_status = request.ingest_status.empty() ? "collected" : request.ingest_status;

    const auto id = result_repository_.create_raw_file(std::move(record));
    return {labbridge::core::Status::success(), id};
}

ResultCreateResult ResultService::record_parsed_record(const RecordParsedRecordRequest& request) {
    if (request.task_run_id.empty()) {
        return {labbridge::core::Status::failure("task_run_id is required"), {}};
    }
    if (request.raw_file_id.empty()) {
        return {labbridge::core::Status::failure("raw_file_id is required"), {}};
    }
    if (request.record.payload_json.empty()) {
        return {labbridge::core::Status::failure("payload_json is required"), {}};
    }

    const auto task_run = task_run_repository_.find_by_id(request.task_run_id);
    if (!task_run.has_value()) {
        return {labbridge::core::Status::failure("task run is not found"), {}};
    }

    const auto raw_file = result_repository_.find_raw_file(request.raw_file_id);
    if (!raw_file.has_value()) {
        return {labbridge::core::Status::failure("raw file is not found"), {}};
    }
    if (raw_file->task_run_id != request.task_run_id) {
        return {labbridge::core::Status::failure("raw file does not belong to task run"), {}};
    }

    ParsedRecordRecord record;
    record.task_run_id = request.task_run_id;
    record.raw_file_id = request.raw_file_id;
    record.record = request.record;
    record.parse_status = request.parse_status.empty() ? "parsed" : request.parse_status;

    const auto id = result_repository_.create_parsed_record(std::move(record));
    return {labbridge::core::Status::success(), id};
}

std::vector<RawFileRecord> ResultService::find_raw_files(const std::string& task_run_id) const {
    if (task_run_id.empty()) {
        return {};
    }
    return result_repository_.find_raw_files_by_run(task_run_id);
}

std::vector<ParsedRecordRecord> ResultService::find_parsed_records(
    const std::string& task_run_id) const {
    if (task_run_id.empty()) {
        return {};
    }
    return result_repository_.find_parsed_records_by_run(task_run_id);
}

}  // namespace labbridge::server
