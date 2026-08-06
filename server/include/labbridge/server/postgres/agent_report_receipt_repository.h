#pragma once

#include "labbridge/server/repositories/agent_report_receipt_repository.h"
#include "labbridge/server/postgres/sql_session.h"

namespace labbridge::server {

class PostgresAgentReportReceiptRepository final
    : public IAgentReportReceiptRepository {
public:
    explicit PostgresAgentReportReceiptRepository(ISqlSession& session);

    AgentReportReceiptClaimResult claim(AgentReportReceiptClaim request) override;
    void complete(const std::string& receipt_id,
                  AgentReportReceiptResponse response) override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
