#include "labbridge/server/postgres/libpq_sql_session.h"

#include <cassert>
#include <stdexcept>
#include <string>

int main() {
    const int version = labbridge::server::LibpqSqlSession::client_version();
    assert(version >= 160000);

    bool failed_to_connect = false;
    try {
        labbridge::server::LibpqSqlSession session{
            "host=127.0.0.1 port=1 dbname=labbridge connect_timeout=1"};
    } catch (const std::runtime_error& error) {
        failed_to_connect = std::string(error.what()).find("failed to connect to PostgreSQL") !=
                            std::string::npos;
    }

    assert(failed_to_connect);
    return 0;
}
