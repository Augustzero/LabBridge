#include "labbridge/server/postgres/libpq_sql_session.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

TEST(LibpqSqlSessionTest, ReportsClientVersionAndConnectionFailure) {
    const int version = labbridge::server::LibpqSqlSession::client_version();
    EXPECT_TRUE(version >= 160000);

    bool failed_to_connect = false;
    try {
        labbridge::server::LibpqSqlSession session{
            "host=127.0.0.1 port=1 dbname=labbridge connect_timeout=1"};
    } catch (const std::runtime_error& error) {
        failed_to_connect = std::string(error.what()).find("failed to connect to PostgreSQL") !=
                            std::string::npos;
    }

    EXPECT_TRUE(failed_to_connect);
}
