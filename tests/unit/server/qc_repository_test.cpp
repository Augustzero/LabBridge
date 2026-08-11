#include "labbridge/server/postgres/qc_repository.h"
#include "support/server/recording_sql_session.h"

#include <gtest/gtest.h>

namespace {

using labbridge::server::PostgresQcRepository;
using labbridge::server::SqlRow;
using labbridge::server::test_support::RecordingSqlSession;

TEST(PostgresQcRepositoryTest, CreateRuleMapsConfiguration) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "1201"}}};
    };
    PostgresQcRepository repository{session};

    const auto id = repository.create_rule({
        {}, "Required fields", "required_fields", "{}", true,
    });

    EXPECT_EQ(id, "1201");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    const auto& statement = session.query_one_calls.front();
    EXPECT_NE(statement.sql.find("INSERT INTO qc_rules"), std::string::npos);
    EXPECT_EQ(statement.params,
              (labbridge::server::SqlParams{
                  "Required fields", "required_fields", "{}", "true"}));
}

TEST(PostgresQcRepositoryTest, FindRuleMapsEnabledFlag) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{
            {"id", "1201"},
            {"name", "Required fields"},
            {"rule_type", "required_fields"},
            {"rule_config_json", "{}"},
            {"enabled", "false"},
        }};
    };
    PostgresQcRepository repository{session};

    const auto rule = repository.find_rule("1201");

    ASSERT_TRUE(rule.has_value());
    EXPECT_EQ(rule->rule_type, "required_fields");
    EXPECT_FALSE(rule->enabled);
}

TEST(PostgresQcRepositoryTest, CreateResultMapsOutcome) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "1301"}}};
    };
    PostgresQcRepository repository{session};

    const auto id = repository.create_result({
        {}, "901", "1201", "failed", "failed", "missing station_code",
    });

    EXPECT_EQ(id, "1301");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    EXPECT_NE(session.query_one_calls.front().sql.find(
                  "INSERT INTO qc_results"),
              std::string::npos);
    EXPECT_EQ(session.query_one_calls.front().params,
              (labbridge::server::SqlParams{
                  "901", "1201", "failed", "failed",
                  "missing station_code"}));
}

TEST(PostgresQcRepositoryTest, FindResultsMapsRows) {
    RecordingSqlSession session;
    session.on_query_all = [](const std::string&, const auto&) {
        return std::vector<SqlRow>{SqlRow{
            {"id", "1301"},
            {"parsed_record_id", "901"},
            {"qc_rule_id", "1201"},
            {"level", "warning"},
            {"result", "warning"},
            {"message", "near upper limit"},
        }};
    };
    PostgresQcRepository repository{session};

    const auto results = repository.find_results_by_parsed_record("901");

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().qc_rule_id, "1201");
    EXPECT_EQ(results.front().level, "warning");
    EXPECT_EQ(results.front().message, "near upper limit");
}

}  // namespace
