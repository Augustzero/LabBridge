#include "labbridge/server/sql_transaction.h"

namespace labbridge::server {

SqlTransaction::SqlTransaction(ISqlSession& session) : session_(session) {
    session_.execute("BEGIN", {});
}

SqlTransaction::~SqlTransaction() {
    if (!active_) {
        return;
    }
    try {
        session_.execute("ROLLBACK", {});
    } catch (...) {
    }
}

void SqlTransaction::commit() {
    session_.execute("COMMIT", {});
    active_ = false;
}

}  // namespace labbridge::server
