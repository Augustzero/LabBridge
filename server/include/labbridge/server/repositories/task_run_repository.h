#pragma once

#include "labbridge/core/models.h"

#include <optional>
#include <string>

namespace labbridge::server {

struct TaskRunRecord {
    std::string id;
    std::string task_id;
    std::string node_code;
    labbridge::core::TaskRunStatus status{labbridge::core::TaskRunStatus::Pending};
    std::string started_at;
    std::string finished_at;
    int items_total{0};
    int items_success{0};
    int items_failed{0};
    std::string error_summary;
    std::string trigger_type{"scheduled"};
    std::string execution_key;
    std::string scheduled_for;
};

struct ScheduledTaskRunStart {
    TaskRunRecord task_run;
    bool created{false};
};

class ITaskRunRepository {
public:
    virtual ~ITaskRunRepository() = default;

    virtual std::string create(TaskRunRecord task_run) = 0;
    virtual ScheduledTaskRunStart create_or_find_scheduled(
        TaskRunRecord task_run) = 0;
    virtual std::optional<TaskRunRecord> find_by_id(const std::string& task_run_id) const = 0;
    virtual void finish(TaskRunRecord task_run) = 0;
};

}  // namespace labbridge::server
