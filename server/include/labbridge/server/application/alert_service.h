#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/repositories/alert_repository.h"
#include "labbridge/server/repositories/qc_repository.h"
#include "labbridge/server/repositories/result_repository.h"
#include "labbridge/server/repositories/task_run_repository.h"

#include <string>
#include <vector>

namespace labbridge::server {

struct CreateAlertFromQcResultRequest {
    std::string qc_result_id;
};

struct AlertCreateResult {
    labbridge::core::Status status;
    std::string id;
};

class AlertService {
public:
    AlertService(ITaskRunRepository& task_run_repository,
                 IResultRepository& result_repository,
                 IQcRepository& qc_repository,
                 IAlertRepository& alert_repository);

    AlertCreateResult create_from_qc_result(const CreateAlertFromQcResultRequest& request);
    AlertCreateResult create_from_qc_result_if_needed(
        const CreateAlertFromQcResultRequest& request);
    std::vector<AlertRecord> find_alerts_by_node(const std::string& node_code) const;
    std::vector<AlertRecord> find_alerts_by_task_run(const std::string& task_run_id) const;

private:
    AlertCreateResult create_alert(const QcResultRecord& qc_result);

    ITaskRunRepository& task_run_repository_;
    IResultRepository& result_repository_;
    IQcRepository& qc_repository_;
    IAlertRepository& alert_repository_;
};

}  // namespace labbridge::server
