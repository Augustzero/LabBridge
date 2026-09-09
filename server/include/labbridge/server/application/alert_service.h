#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/repositories/alert_repository.h"
#include "labbridge/server/repositories/qc_repository.h"
#include "labbridge/server/repositories/result_repository.h"
#include "labbridge/server/repositories/task_run_repository.h"

#include <string>

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

    // 上报链路已持有完整 qc 结果与归属信息，直接落库；
    // 结果不需要告警时返回成功且 id 为空。
    AlertCreateResult create_alert(const QcResultRecord& qc_result,
                                   const std::string& task_run_id,
                                   const std::string& node_code);

private:
    // 共享实现：按 qc_result_id 回读并判断是否产生告警，only_fail 决定
    // “不产生告警”返回 Conflict 还是成功空返回。
    AlertCreateResult create_from_lookup(const CreateAlertFromQcResultRequest& request,
                                         bool only_fail);
    AlertCreateResult write_alert(const QcResultRecord& qc_result,
                                  const std::string& task_run_id,
                                  const std::string& node_code);

    ITaskRunRepository& task_run_repository_;
    IResultRepository& result_repository_;
    IQcRepository& qc_repository_;
    IAlertRepository& alert_repository_;
};

}  // namespace labbridge::server
