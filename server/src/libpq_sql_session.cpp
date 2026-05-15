#include "labbridge/server/libpq_sql_session.h"

#include <stdexcept>
#include <vector>

namespace labbridge::server {
namespace {

std::vector<const char*> make_param_values(const SqlParams& params) {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& param : params) {
        values.push_back(param.c_str());
    }
    return values;
}

std::runtime_error make_error(PGconn* connection, const std::string& prefix) {
    const char* message = connection == nullptr ? "no PostgreSQL connection" : PQerrorMessage(connection);
    return std::runtime_error(prefix + ": " + message);
}

class ResultGuard {
public:
    explicit ResultGuard(PGresult* result) : result_(result) {}
    ~ResultGuard() {
        if (result_ != nullptr) {
            PQclear(result_);
        }
    }

    ResultGuard(const ResultGuard&) = delete;
    ResultGuard& operator=(const ResultGuard&) = delete;

    PGresult* get() const {
        return result_;
    }

private:
    PGresult* result_{nullptr};
};

}  // namespace

LibpqSqlSession::LibpqSqlSession(const std::string& connection_info) {
    connection_ = PQconnectdb(connection_info.c_str());
    if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
        auto error = make_error(connection_, "failed to connect to PostgreSQL");
        if (connection_ != nullptr) {
            PQfinish(connection_);
            connection_ = nullptr;
        }
        throw error;
    }
}

LibpqSqlSession::~LibpqSqlSession() {
    if (connection_ != nullptr) {
        PQfinish(connection_);
    }
}

void LibpqSqlSession::execute(const std::string& sql, const SqlParams& params) {
    const auto values = make_param_values(params);
    ResultGuard result(PQexecParams(connection_,
                                    sql.c_str(),
                                    static_cast<int>(values.size()),
                                    nullptr,
                                    values.data(),
                                    nullptr,
                                    nullptr,
                                    0));

    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
        throw make_error(connection_, "failed to execute PostgreSQL command");
    }
}

std::optional<SqlRow> LibpqSqlSession::query_one(const std::string& sql, const SqlParams& params) {
    const auto values = make_param_values(params);
    ResultGuard result(PQexecParams(connection_,
                                    sql.c_str(),
                                    static_cast<int>(values.size()),
                                    nullptr,
                                    values.data(),
                                    nullptr,
                                    nullptr,
                                    0));

    if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        throw make_error(connection_, "failed to execute PostgreSQL query");
    }

    if (PQntuples(result.get()) == 0) {
        return std::nullopt;
    }

    SqlRow row;
    for (int column = 0; column < PQnfields(result.get()); ++column) {
        const char* name = PQfname(result.get(), column);
        if (name == nullptr) {
            continue;
        }

        if (PQgetisnull(result.get(), 0, column) != 0) {
            row[name] = "";
            continue;
        }

        row[name] = PQgetvalue(result.get(), 0, column);
    }
    return row;
}

int LibpqSqlSession::client_version() {
    return PQlibVersion();
}

}  // namespace labbridge::server
