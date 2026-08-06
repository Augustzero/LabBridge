#include "labbridge/server/postgres/agent_report_executor.h"

#include "labbridge/server/application/alert_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/agent_report_receipt_repository.h"
#include "labbridge/server/postgres/alert_repository.h"
#include "labbridge/server/postgres/config_repository.h"
#include "labbridge/server/postgres/qc_repository.h"
#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/task_run_repository.h"
#include "labbridge/server/application/qc_service.h"
#include "labbridge/server/application/result_service.h"
#include "labbridge/server/postgres/sql_transaction.h"
#include "labbridge/server/application/task_run_service.h"

#include <utility>

namespace labbridge::server {
namespace {

class AgentReportRequestScope {
public:
    explicit AgentReportRequestScope(const std::string& connection_info)
        : session_(connection_info),
          config_repository_(session_),
          task_run_repository_(session_),
          result_repository_(session_),
          qc_repository_(session_),
          alert_repository_(session_),
          receipt_repository_(session_),
          task_run_service_(config_repository_, task_run_repository_),
          result_service_(task_run_repository_, result_repository_),
          qc_service_(result_repository_, qc_repository_),
          alert_service_(
              task_run_repository_, result_repository_, qc_repository_, alert_repository_),
          agent_report_service_(
              task_run_service_,
              result_service_,
              qc_service_,
              alert_service_,
              receipt_repository_) {}

    ISqlSession& session() {
        return session_;
    }

    AgentReportService& agent_report_service() {
        return agent_report_service_;
    }

private:
    LibpqSqlSession session_;
    PostgresConfigRepository config_repository_;
    PostgresTaskRunRepository task_run_repository_;
    PostgresResultRepository result_repository_;
    PostgresQcRepository qc_repository_;
    PostgresAlertRepository alert_repository_;
    PostgresAgentReportReceiptRepository receipt_repository_;
    TaskRunService task_run_service_;
    ResultService result_service_;
    QcService qc_service_;
    AlertService alert_service_;
    AgentReportService agent_report_service_;
};

}  // namespace

PostgresAgentReportExecutor::PostgresAgentReportExecutor(std::string connection_info)
    : connection_info_(std::move(connection_info)) {}

RawFileManifestResult PostgresAgentReportExecutor::accept_raw_file_manifest(
    const RawFileManifestRequest& request) const {
    AgentReportRequestScope scope{connection_info_};
    SqlTransaction transaction{scope.session()};
    auto result = scope.agent_report_service().accept_raw_file_manifest(request);
    if (result.status.ok) {
        transaction.commit();
    }
    return result;
}

TaskRunReportResult PostgresAgentReportExecutor::accept_task_run_report(
    const TaskRunReportRequest& request) const {
    AgentReportRequestScope scope{connection_info_};
    SqlTransaction transaction{scope.session()};
    auto result = scope.agent_report_service().accept_task_run_report(request);
    if (result.status.ok) {
        transaction.commit();
    }
    return result;
}

}  // namespace labbridge::server
