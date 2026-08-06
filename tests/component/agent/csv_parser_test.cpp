#include "labbridge/agent/parsers/csv_parser.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;

class TemporaryCsvFile final {
public:
    explicit TemporaryCsvFile(const std::string& contents) {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()) +
            "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        path_ = std::filesystem::temp_directory_path() /
                ("labbridge-csv-parser-" + suffix + ".csv");

        std::ofstream output{path_};
        if (!output.is_open()) {
            throw std::runtime_error("failed to create temporary csv file");
        }
        output << contents;
        if (!output.good()) {
            throw std::runtime_error("failed to write temporary csv file");
        }
    }

    ~TemporaryCsvFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryCsvFile(const TemporaryCsvFile&) = delete;
    TemporaryCsvFile& operator=(const TemporaryCsvFile&) = delete;

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

labbridge::agent::ParseResult parse_file(const std::string& path) {
    labbridge::agent::CsvObservationParser parser;
    return parser.parse({"run-test", "raw-test", path});
}

TEST(CsvObservationParserTest, ParsesObservationRowsAndPayloadFields) {
    const auto result =
        parse_file("tests/fixtures/agent/sample_observation.csv");

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_EQ(result.records.size(), 2U);
    EXPECT_TRUE(result.errors.empty());

    const auto& first = result.records.front();
    EXPECT_EQ(first.station_code, "ST001");
    EXPECT_EQ(first.device_code, "DV001");
    EXPECT_EQ(first.record_time, "2026-05-09 10:00:00");
    const auto payload = Json::parse(first.payload_json);
    EXPECT_EQ(payload.at("temperature"), "22.5");
    EXPECT_EQ(payload.at("humidity"), "57");
}

TEST(CsvObservationParserTest, ReturnsFailureForMissingFile) {
    const auto result = parse_file(
        "tests/fixtures/agent/does-not-exist.csv");

    EXPECT_FALSE(result.status.ok);
    EXPECT_FALSE(result.status.message.empty());
    EXPECT_TRUE(result.records.empty());
}

TEST(CsvObservationParserTest, ReturnsFailureForEmptyFile) {
    TemporaryCsvFile file{""};

    const auto result = parse_file(file.path().string());

    EXPECT_FALSE(result.status.ok);
    EXPECT_FALSE(result.status.message.empty());
    EXPECT_TRUE(result.records.empty());
}

TEST(CsvObservationParserTest, ReturnsFailureForShortHeader) {
    TemporaryCsvFile file{"station_code,device_code,record_time\n"};

    const auto result = parse_file(file.path().string());

    EXPECT_FALSE(result.status.ok);
    EXPECT_FALSE(result.status.message.empty());
    EXPECT_TRUE(result.records.empty());
}

TEST(CsvObservationParserTest, ReportsShortRowsAndContinuesParsing) {
    TemporaryCsvFile file{
        "station_code,device_code,record_time,value\n"
        "station-a,device-a\n"
        "\n"
        "station-b,device-b,2026-07-31 10:15:30,42\n"};

    const auto result = parse_file(file.path().string());

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_NE(result.errors.front().find("line 2"), std::string::npos);
    ASSERT_EQ(result.records.size(), 1U);
    EXPECT_EQ(result.records.front().station_code, "station-b");
}

TEST(CsvObservationParserTest, ProducesValidJsonForEscapedPayloadText) {
    const auto result =
        parse_file("tests/fixtures/agent/escaped_observation.csv");

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_EQ(result.records.size(), 1U);
    const auto payload = Json::parse(result.records.front().payload_json);
    EXPECT_EQ(payload.at("note"), "\"quoted\"");
    EXPECT_EQ(payload.at("path"), "C:\\temp");
}

}  // namespace
