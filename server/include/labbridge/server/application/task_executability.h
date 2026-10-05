#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/repositories/config_repository.h"
#include "labbridge/server/repositories/node_repository.h"
#include "labbridge/server/repositories/qc_repository.h"

#include <string>
#include <vector>

namespace labbridge::server {

// 校验存量依赖（node / data source / QC rule）是否可执行。
// create_task / enable_task 与人工执行受理共用同一套判定，
// 保证“能创建的任务”和“能手动跑的任务”口径一致。
labbridge::core::Status validate_task_executable(
    INodeRepository& node_repository,
    IConfigRepository& config_repository,
    IQcRepository& qc_repository,
    const TaskRecord& task,
    const std::vector<std::string>& qc_rule_ids);

}  // namespace labbridge::server
