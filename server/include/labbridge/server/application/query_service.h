#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/repositories/alert_repository.h"
#include "labbridge/server/repositories/config_repository.h"
#include "labbridge/server/repositories/node_repository.h"
#include "labbridge/server/repositories/qc_repository.h"
#include "labbridge/server/repositories/result_repository.h"
#include "labbridge/server/repositories/task_run_repository.h"

#include <optional>
#include <string>
#include <vector>

namespace labbridge::server {

struct NodeOverviewQueryResult {
    labbridge::core::Status status;
    std::optional<NodeRecord> node;
    std::vector<TaskRecord> enabled_tasks;
    std::vector<AlertRecord> alerts;
};

struct TaskRunDetailQueryResult {
    labbridge::core::Status status;
    std::optional<TaskRunRecord> task_run;
    std::vector<RawFileRecord> raw_files;
    std::vector<ParsedRecordRecord> parsed_records;
    std::vector<QcResultRecord> qc_results;
    std::vector<AlertRecord> alerts;
};

class ControlPlaneQueryService {
public:
    ControlPlaneQueryService(INodeRepository& node_repository,
                             IConfigRepository& config_repository,
                             ITaskRunRepository& task_run_repository,
                             IResultRepository& result_repository,
                             IQcRepository& qc_repository,
                             IAlertRepository& alert_repository);

    NodeOverviewQueryResult find_node_overview(const std::string& node_code) const;
    TaskRunDetailQueryResult find_task_run_detail(const std::string& node_code,
                                                  const std::string& task_run_id) const;

private:
    INodeRepository& node_repository_;
    IConfigRepository& config_repository_;
    ITaskRunRepository& task_run_repository_;
    IResultRepository& result_repository_;
    IQcRepository& qc_repository_;
    IAlertRepository& alert_repository_;
};

}  // namespace labbridge::server
