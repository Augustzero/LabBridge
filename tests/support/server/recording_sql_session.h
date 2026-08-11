#pragma once

#include "labbridge/server/postgres/sql_session.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace labbridge::server::test_support {

struct RecordedSqlStatement {
    std::string sql;
    SqlParams params;
};

class RecordingSqlSession final : public ISqlSession {
public:
    using ExecuteHandler =
        std::function<void(const std::string&, const SqlParams&)>;
    using QueryOneHandler = std::function<std::optional<SqlRow>(
        const std::string&, const SqlParams&)>;
    using QueryAllHandler = std::function<std::vector<SqlRow>(
        const std::string&, const SqlParams&)>;

    void execute(const std::string& sql, const SqlParams& params) override {
        executions.push_back({sql, params});
        if (on_execute) {
            on_execute(sql, params);
        }
    }

    std::optional<SqlRow> query_one(
        const std::string& sql,
        const SqlParams& params) override {
        query_one_calls.push_back({sql, params});
        if (on_query_one) {
            return on_query_one(sql, params);
        }
        return std::nullopt;
    }

    std::vector<SqlRow> query_all(
        const std::string& sql,
        const SqlParams& params) override {
        query_all_calls.push_back({sql, params});
        if (on_query_all) {
            return on_query_all(sql, params);
        }
        return {};
    }

    ExecuteHandler on_execute;
    QueryOneHandler on_query_one;
    QueryAllHandler on_query_all;
    std::vector<RecordedSqlStatement> executions;
    std::vector<RecordedSqlStatement> query_one_calls;
    std::vector<RecordedSqlStatement> query_all_calls;
};

}  // namespace labbridge::server::test_support
