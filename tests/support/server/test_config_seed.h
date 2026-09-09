#pragma once

#include "labbridge/server/repositories/config_repository.h"

#include <string>
#include <utility>

namespace labbridge::server::test_support {

// 测试通用的配置种子：直接经 repository 写入，
// 绕过已被删除的 ConfigService create 入口。
inline std::string create_local_csv_data_source(
    IConfigRepository& repository,
    const std::string& node_code,
    const std::string& name,
    const std::string& config_json = "{}",
    bool enabled = true) {
    DataSourceRecord record;
    record.node_code = node_code;
    record.source_type = labbridge::core::SourceType::LocalDirectory;
    record.name = name;
    record.config_json = config_json;
    record.enabled = enabled;
    return repository.create_data_source(std::move(record));
}

inline std::string create_csv_task(
    IConfigRepository& repository,
    const std::string& node_code,
    const std::string& data_source_id,
    const std::string& name,
    bool enabled = true) {
    TaskRecord record;
    record.node_code = node_code;
    record.data_source_id = data_source_id;
    record.name = name;
    record.task_type = "local_file_import";
    record.schedule_expr = "* * * * *";
    record.parser_type = "csv_observation";
    record.enabled = enabled;
    return repository.create_task(std::move(record));
}

}  // namespace labbridge::server::test_support
