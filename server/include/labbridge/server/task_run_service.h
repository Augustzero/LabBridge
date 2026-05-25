#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/config_repository.h"
#include "labbridge/server/task_run_repository.h"

#include <optional>
#include <string>

namespace labbridge::server {

struct StartTaskRunRequest {
    std::string node_code;
    std::string task_id;
    std::string started_at;
    std::string trigger_type{"scheduled"};
};

struct FinishTaskRunRequest {
    std::string task_run_id;
    labbridge::core::TaskRunStatus status{labbridge::core::TaskRunStatus::Succeeded};
    std::string finished_at;
    int items_total{0};
    int items_success{0};
    int items_failed{0};
    std::string error_summary;
};

struct TaskRunCreateResult {
    labbridge::core::Status status;
    std::string id;
};

class TaskRunService {
public:
    TaskRunService(IConfigRepository& config_repository, ITaskRunRepository& task_run_repository);

    TaskRunCreateResult start(const StartTaskRunRequest& request);
    labbridge::core::Status finish(const FinishTaskRunRequest& request);
    std::optional<TaskRunRecord> find_run(const std::string& task_run_id) const;

private:
    IConfigRepository& config_repository_;
    ITaskRunRepository& task_run_repository_;
};

}  // namespace labbridge::server
