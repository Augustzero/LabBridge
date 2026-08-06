#pragma once

#include "labbridge/server/application/agent_report_service.h"

#include <string>

namespace labbridge::server {

class PostgresAgentReportExecutor {
public:
    explicit PostgresAgentReportExecutor(std::string connection_info);

    RawFileManifestResult accept_raw_file_manifest(
        const RawFileManifestRequest& request) const;
    TaskRunReportResult accept_task_run_report(const TaskRunReportRequest& request) const;

private:
    std::string connection_info_;
};

}  // namespace labbridge::server
