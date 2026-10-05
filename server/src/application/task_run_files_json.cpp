#include "labbridge/server/repositories/task_run_repository.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace labbridge::server {
namespace {

using Json = nlohmann::json;

Json encode_failed_file(const TaskRunFailedFile& file) {
    return Json{
        {"source_path", file.source_path},
        {"original_name", file.original_name},
        {"stage", file.stage},
        {"message", file.message},
        {"archive_raw_file_id", file.archive_raw_file_id},
    };
}

Json encode_retry_file(const TaskRunRetryFile& file) {
    return Json{
        {"input_type", file.input_type},
        {"source_path", file.source_path},
        {"original_name", file.original_name},
        {"archive_raw_file_id", file.archive_raw_file_id},
        {"storage_path", file.storage_path},
        {"size_bytes", file.size_bytes},
        {"file_hash", file.file_hash},
        {"source_mtime", file.source_mtime},
    };
}

std::string required_string(const Json& payload, const char* field) {
    const auto iterator = payload.find(field);
    if (iterator == payload.end() || !iterator->is_string()) {
        throw std::runtime_error(std::string{"task run file field '"} + field +
                                 "' must be a string");
    }
    return iterator->get<std::string>();
}

long long required_size(const Json& payload) {
    const auto iterator = payload.find("size_bytes");
    if (iterator == payload.end() || !iterator->is_number_unsigned()) {
        throw std::runtime_error("retry file field 'size_bytes' must be a non-negative integer");
    }
    return iterator->get<long long>();
}

}  // namespace

std::string to_failed_files_json(const std::vector<TaskRunFailedFile>& files) {
    Json array = Json::array();
    for (const auto& file : files) {
        array.push_back(encode_failed_file(file));
    }
    return array.dump();
}

std::optional<std::vector<TaskRunFailedFile>> parse_failed_files_json(
    const std::string& json) {
    if (json.empty()) {
        return std::nullopt;
    }
    try {
        const auto payload = Json::parse(json);
        if (!payload.is_array()) {
            return std::nullopt;
        }
        std::vector<TaskRunFailedFile> files;
        files.reserve(payload.size());
        for (const auto& item : payload) {
            if (!item.is_object()) {
                return std::nullopt;
            }
            TaskRunFailedFile file;
            file.source_path = required_string(item, "source_path");
            file.original_name = required_string(item, "original_name");
            file.stage = required_string(item, "stage");
            file.message = required_string(item, "message");
            file.archive_raw_file_id =
                required_string(item, "archive_raw_file_id");
            files.push_back(std::move(file));
        }
        return files;
    } catch (const Json::exception&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::string to_retry_files_json(const std::vector<TaskRunRetryFile>& files) {
    Json array = Json::array();
    for (const auto& file : files) {
        array.push_back(encode_retry_file(file));
    }
    return array.dump();
}

std::optional<std::vector<TaskRunRetryFile>> parse_retry_files_json(
    const std::string& json) {
    if (json.empty()) {
        return std::nullopt;
    }
    try {
        const auto payload = Json::parse(json);
        if (!payload.is_array()) {
            return std::nullopt;
        }
        std::vector<TaskRunRetryFile> files;
        files.reserve(payload.size());
        for (const auto& item : payload) {
            if (!item.is_object()) {
                return std::nullopt;
            }
            TaskRunRetryFile file;
            file.input_type = required_string(item, "input_type");
            if (file.input_type != "archive" && file.input_type != "source") {
                return std::nullopt;
            }
            file.source_path = required_string(item, "source_path");
            file.original_name = required_string(item, "original_name");
            file.archive_raw_file_id =
                required_string(item, "archive_raw_file_id");
            file.storage_path = required_string(item, "storage_path");
            file.size_bytes = required_size(item);
            file.file_hash = required_string(item, "file_hash");
            file.source_mtime = required_string(item, "source_mtime");
            files.push_back(std::move(file));
        }
        return files;
    } catch (const Json::exception&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

}  // namespace labbridge::server
