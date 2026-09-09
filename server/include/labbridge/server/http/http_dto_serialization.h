#pragma once

#include "labbridge/core/models.h"

#include <json/reader.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace labbridge::server::http {

// http 层共享的 DTO 序列化小工具：多个控制器完全一致的实现。

inline Json::Value stored_json_object(const std::string& text,
                                      const std::string& field) {
    Json::CharReaderBuilder builder;
    auto reader = std::unique_ptr<Json::CharReader>{builder.newCharReader()};
    Json::Value value;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors) ||
        !value.isObject()) {
        throw std::runtime_error(field + " must contain a JSON object");
    }
    return value;
}

inline std::string node_status(labbridge::core::NodeStatus status) {
    return status == labbridge::core::NodeStatus::Online ? "online" : "offline";
}

inline std::string source_type(labbridge::core::SourceType type) {
    switch (type) {
        case labbridge::core::SourceType::LocalDirectory:
            return "local_directory";
        case labbridge::core::SourceType::Ftp:
            return "ftp";
        case labbridge::core::SourceType::Oracle:
            return "oracle";
    }
    throw std::runtime_error("unsupported data source type");
}

}  // namespace labbridge::server::http
