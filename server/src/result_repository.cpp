#include "labbridge/server/result_repository.h"

#include <utility>

namespace labbridge::server {

std::string InMemoryResultRepository::create_raw_file(RawFileRecord raw_file) {
    if (raw_file.id.empty()) {
        raw_file.id = std::to_string(next_raw_file_id_++);
    }

    const auto id = raw_file.id;
    raw_files_[id] = std::move(raw_file);
    return id;
}

std::optional<RawFileRecord> InMemoryResultRepository::find_raw_file(
    const std::string& raw_file_id) const {
    const auto iter = raw_files_.find(raw_file_id);
    if (iter == raw_files_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

std::vector<RawFileRecord> InMemoryResultRepository::find_raw_files_by_run(
    const std::string& task_run_id) const {
    std::vector<RawFileRecord> records;
    for (const auto& [id, record] : raw_files_) {
        if (record.task_run_id == task_run_id) {
            records.push_back(record);
        }
    }
    return records;
}

std::string InMemoryResultRepository::create_parsed_record(ParsedRecordRecord parsed_record) {
    if (parsed_record.id.empty()) {
        parsed_record.id = std::to_string(next_parsed_record_id_++);
    }

    const auto id = parsed_record.id;
    parsed_records_[id] = std::move(parsed_record);
    return id;
}

std::optional<ParsedRecordRecord> InMemoryResultRepository::find_parsed_record(
    const std::string& parsed_record_id) const {
    const auto iter = parsed_records_.find(parsed_record_id);
    if (iter == parsed_records_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

std::vector<ParsedRecordRecord> InMemoryResultRepository::find_parsed_records_by_run(
    const std::string& task_run_id) const {
    std::vector<ParsedRecordRecord> records;
    for (const auto& [id, record] : parsed_records_) {
        if (record.task_run_id == task_run_id) {
            records.push_back(record);
        }
    }
    return records;
}

}  // namespace labbridge::server
