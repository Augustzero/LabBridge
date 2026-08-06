#pragma once

#include "labbridge/core/models.h"
#include "labbridge/server/postgres/sql_session.h"

#include <stdexcept>
#include <string>

namespace labbridge::server::storage {

inline std::string value_or_empty(const SqlRow& row, const std::string& key) {
    const auto iter = row.find(key);
    if (iter == row.end()) {
        return {};
    }
    return iter->second;
}

inline int int_or_zero(const SqlRow& row, const std::string& key) {
    const auto value = value_or_empty(row, key);
    if (value.empty()) {
        return 0;
    }
    return std::stoi(value);
}

inline long long int64_or_zero(const SqlRow& row, const std::string& key) {
    const auto value = value_or_empty(row, key);
    if (value.empty()) {
        return 0;
    }
    return std::stoll(value);
}

inline std::string bool_param(bool value) {
    return value ? "true" : "false";
}

inline bool bool_value(const std::string& value) {
    if (value == "true" || value == "t" || value == "1") {
        return true;
    }
    if (value == "false" || value == "f" || value == "0") {
        return false;
    }
    throw std::runtime_error("unsupported boolean storage value");
}

inline std::string to_storage(labbridge::core::NodeStatus status) {
    switch (status) {
        case labbridge::core::NodeStatus::Online:
            return "online";
        case labbridge::core::NodeStatus::Offline:
            return "offline";
    }
    throw std::runtime_error("unsupported node status");
}

inline labbridge::core::NodeStatus node_status_from_storage(
    const std::string& status) {
    if (status == "online") {
        return labbridge::core::NodeStatus::Online;
    }
    if (status == "offline") {
        return labbridge::core::NodeStatus::Offline;
    }
    throw std::runtime_error("unsupported node status storage value");
}

inline std::string to_storage(labbridge::core::TaskRunStatus status) {
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

inline labbridge::core::TaskRunStatus task_run_status_from_storage(
    const std::string& status) {
    if (status == "pending") {
        return labbridge::core::TaskRunStatus::Pending;
    }
    if (status == "running") {
        return labbridge::core::TaskRunStatus::Running;
    }
    if (status == "succeeded") {
        return labbridge::core::TaskRunStatus::Succeeded;
    }
    if (status == "failed") {
        return labbridge::core::TaskRunStatus::Failed;
    }
    throw std::runtime_error("unsupported task run status storage value");
}

inline std::string to_storage(labbridge::core::SourceType source_type) {
    switch (source_type) {
        case labbridge::core::SourceType::LocalDirectory:
            return "local_directory";
        case labbridge::core::SourceType::Ftp:
            return "ftp";
        case labbridge::core::SourceType::Oracle:
            return "oracle";
    }
    throw std::runtime_error("unsupported source type");
}

inline labbridge::core::SourceType source_type_from_storage(
    const std::string& source_type) {
    if (source_type == "local_directory") {
        return labbridge::core::SourceType::LocalDirectory;
    }
    if (source_type == "ftp") {
        return labbridge::core::SourceType::Ftp;
    }
    if (source_type == "oracle") {
        return labbridge::core::SourceType::Oracle;
    }
    throw std::runtime_error("unsupported source type storage value");
}

}  // namespace labbridge::server::storage
