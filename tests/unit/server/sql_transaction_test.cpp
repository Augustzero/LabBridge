#include "labbridge/server/postgres/sql_transaction.h"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class RecordingSqlSession final : public labbridge::server::ISqlSession {
public:
    void execute(const std::string& sql,
                 const labbridge::server::SqlParams&) override {
        statements.push_back(sql);
        if (sql == failing_statement) {
            throw std::runtime_error("planned SQL failure");
        }
    }

    std::optional<labbridge::server::SqlRow> query_one(
        const std::string&,
        const labbridge::server::SqlParams&) override {
        return std::nullopt;
    }

    std::vector<labbridge::server::SqlRow> query_all(
        const std::string&,
        const labbridge::server::SqlParams&) override {
        return {};
    }

    std::string failing_statement;
    std::vector<std::string> statements;
};

TEST(SqlTransactionTest, BeginsAndRollsBackUncommittedTransaction) {
    RecordingSqlSession session;

    { labbridge::server::SqlTransaction transaction{session}; }

    EXPECT_EQ(session.statements,
              (std::vector<std::string>{"BEGIN", "ROLLBACK"}));
}

TEST(SqlTransactionTest, CommitPreventsDestructorRollback) {
    RecordingSqlSession session;

    {
        labbridge::server::SqlTransaction transaction{session};
        transaction.commit();
    }

    EXPECT_EQ(session.statements,
              (std::vector<std::string>{"BEGIN", "COMMIT"}));
}

TEST(SqlTransactionTest, PropagatesBeginFailureWithoutRollback) {
    RecordingSqlSession session;
    session.failing_statement = "BEGIN";

    EXPECT_THROW(
        { labbridge::server::SqlTransaction transaction{session}; },
        std::runtime_error);

    EXPECT_EQ(session.statements, (std::vector<std::string>{"BEGIN"}));
}

TEST(SqlTransactionTest, CommitFailureLeavesTransactionActiveForRollback) {
    RecordingSqlSession session;
    session.failing_statement = "COMMIT";

    {
        labbridge::server::SqlTransaction transaction{session};
        EXPECT_THROW(transaction.commit(), std::runtime_error);
    }

    EXPECT_EQ(session.statements,
              (std::vector<std::string>{"BEGIN", "COMMIT", "ROLLBACK"}));
}

TEST(SqlTransactionTest, DestructorSuppressesRollbackFailure) {
    RecordingSqlSession session;
    session.failing_statement = "ROLLBACK";

    EXPECT_NO_THROW(
        { labbridge::server::SqlTransaction transaction{session}; });

    EXPECT_EQ(session.statements,
              (std::vector<std::string>{"BEGIN", "ROLLBACK"}));
}

}  // namespace
