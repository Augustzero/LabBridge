#pragma once

#include "labbridge/server/postgres/sql_session.h"

#include <libpq-fe.h>

#include <string>

namespace labbridge::server {

class LibpqSqlSession final : public ISqlSession {
public:
    explicit LibpqSqlSession(const std::string& connection_info);
    ~LibpqSqlSession() override;

    LibpqSqlSession(const LibpqSqlSession&) = delete;
    LibpqSqlSession& operator=(const LibpqSqlSession&) = delete;

    LibpqSqlSession(LibpqSqlSession&&) = delete;
    LibpqSqlSession& operator=(LibpqSqlSession&&) = delete;

    void execute(const std::string& sql, const SqlParams& params) override;
    std::optional<SqlRow> query_one(const std::string& sql, const SqlParams& params) override;
    std::vector<SqlRow> query_all(const std::string& sql, const SqlParams& params) override;

    static int client_version();

private:
    PGconn* connection_{nullptr};
};

}  // namespace labbridge::server
