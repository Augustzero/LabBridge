#include "labbridge/server/postgres/result_repository.h"
#include "support/server/recording_sql_session.h"

#include <gtest/gtest.h>

namespace {

using labbridge::server::PostgresResultRepository;
using labbridge::server::SqlRow;
using labbridge::server::test_support::RecordingSqlSession;

TEST(PostgresResultRepositoryTest, CreateRawFileMapsEvidenceFields) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "801"}}};
    };
    PostgresResultRepository repository{session};

    const auto id = repository.create_raw_file({
        {}, "501", "unit-node", "sample.csv", "sha256",
        "/archive/sample.csv", 320, "2026-08-11T10:00:00Z", "archived_local",
    });

    EXPECT_EQ(id, "801");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    const auto& statement = session.query_one_calls.front();
    EXPECT_NE(statement.sql.find("INSERT INTO raw_files"), std::string::npos);
    EXPECT_EQ(statement.params,
              (labbridge::server::SqlParams{
                  "501", "unit-node", "sample.csv", "sha256",
                  "/archive/sample.csv", "320",
                  "2026-08-11T10:00:00Z", "archived_local"}));
}

TEST(PostgresResultRepositoryTest, FindRawFileMapsStorageRow) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{
            {"id", "801"},
            {"task_run_id", "501"},
            {"node_code", "unit-node"},
            {"original_name", "sample.csv"},
            {"file_hash", "sha256"},
            {"storage_path", "/archive/sample.csv"},
            {"size_bytes", "320"},
            {"source_mtime", "2026-08-11 10:00:00"},
            {"ingest_status", "archived_local"},
        }};
    };
    PostgresResultRepository repository{session};

    const auto raw_file = repository.find_raw_file("801");

    ASSERT_TRUE(raw_file.has_value());
    EXPECT_EQ(raw_file->task_run_id, "501");
    EXPECT_EQ(raw_file->size_bytes, 320);
    EXPECT_EQ(raw_file->storage_path, "/archive/sample.csv");
    EXPECT_EQ(raw_file->ingest_status, "archived_local");
}

TEST(PostgresResultRepositoryTest, CreateParsedRecordMapsObservationFields) {
    RecordingSqlSession session;
    session.on_query_one = [](const std::string&, const auto&) {
        return std::optional<SqlRow>{SqlRow{{"id", "901"}}};
    };
    PostgresResultRepository repository{session};
    labbridge::server::ParsedRecordRecord record;
    record.raw_file_id = "801";
    record.task_run_id = "501";
    record.record.station_code = "station-a";
    record.record.device_code = "device-a";
    record.record.record_time = "2026-08-11T10:00:00Z";
    record.record.payload_json = R"({"temperature":21.5})";
    record.parse_status = "parsed";

    const auto id = repository.create_parsed_record(record);

    EXPECT_EQ(id, "901");
    ASSERT_EQ(session.query_one_calls.size(), 1U);
    const auto& statement = session.query_one_calls.front();
    EXPECT_NE(statement.sql.find("INSERT INTO parsed_records"),
              std::string::npos);
    ASSERT_EQ(statement.params.size(), 7U);
    EXPECT_EQ(statement.params[0], "801");
    EXPECT_EQ(statement.params[5], R"({"temperature":21.5})");
}

TEST(PostgresResultRepositoryTest, FindParsedRecordsMapsRows) {
    RecordingSqlSession session;
    session.on_query_all = [](const std::string&, const auto&) {
        return std::vector<SqlRow>{SqlRow{
            {"id", "901"},
            {"raw_file_id", "801"},
            {"task_run_id", "501"},
            {"station_code", "station-a"},
            {"device_code", "device-a"},
            {"record_time", "2026-08-11 10:00:00"},
            {"payload_json", R"({"temperature":21.5})"},
            {"parse_status", "parsed"},
        }};
    };
    PostgresResultRepository repository{session};

    const auto records = repository.find_parsed_records_by_run("501");

    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records.front().raw_file_id, "801");
    EXPECT_EQ(records.front().record.station_code, "station-a");
    EXPECT_EQ(records.front().record.payload_json,
              R"({"temperature":21.5})");
}

}  // namespace
