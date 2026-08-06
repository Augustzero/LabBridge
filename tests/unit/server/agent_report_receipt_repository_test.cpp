#include "labbridge/server/postgres/agent_report_receipt_repository.h"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class CountingSqlSession final : public labbridge::server::ISqlSession {
public:
    void execute(const std::string&,
                 const labbridge::server::SqlParams&) override {
        ++calls;
    }

    std::optional<labbridge::server::SqlRow> query_one(
        const std::string&,
        const labbridge::server::SqlParams&) override {
        ++calls;
        return std::nullopt;
    }

    std::vector<labbridge::server::SqlRow> query_all(
        const std::string&,
        const labbridge::server::SqlParams&) override {
        ++calls;
        return {};
    }

    int calls{0};
};

TEST(AgentReportReceiptRepositoryTest,
     RejectsUnknownRequestTypeBeforeIssuingSql) {
    CountingSqlSession session;
    labbridge::server::PostgresAgentReportReceiptRepository repository{
        session};
    labbridge::server::AgentReportReceiptClaim request;
    request.task_run_id = "1";
    request.node_code = "node-a";
    request.request_type =
        static_cast<labbridge::server::AgentReportRequestType>(99);
    request.idempotency_key = "key";
    request.request_fingerprint = "fingerprint";

    EXPECT_THROW(
        static_cast<void>(repository.claim(request)),
        std::runtime_error);
    EXPECT_EQ(session.calls, 0);
}

}  // namespace
