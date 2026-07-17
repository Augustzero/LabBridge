#pragma once

#include "labbridge/server/sql_session.h"

namespace labbridge::server {

class SqlTransaction {
public:
    explicit SqlTransaction(ISqlSession& session);
    ~SqlTransaction();

    SqlTransaction(const SqlTransaction&) = delete;
    SqlTransaction& operator=(const SqlTransaction&) = delete;
    SqlTransaction(SqlTransaction&&) = delete;
    SqlTransaction& operator=(SqlTransaction&&) = delete;

    void commit();

private:
    ISqlSession& session_;
    bool active_{true};
};

}  // namespace labbridge::server
