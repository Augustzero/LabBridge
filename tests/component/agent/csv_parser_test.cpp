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
    // """quoted""" 按 CSV 引号语义解出带字面引号的 "quoted"。
    EXPECT_EQ(payload.at("note"), "\"quoted\"");
    EXPECT_EQ(payload.at("path"), "C:\\temp");
}

TEST(CsvObservationParserTest, AcceptsBomAndCrlfLineEndings) {
    TemporaryCsvFile file{
        "\xEF\xBB\xBFstation_code,device_code,record_time,value\r\n"
        "ST001,DV001,2026-08-08 08:00:00,42\r\n"
        "\r\n"};

    const auto result = parse_file(file.path().string());

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_EQ(result.records.size(), 1U);
    EXPECT_EQ(result.records.front().payload_json,
              R"({"value":"42"})");
}

TEST(CsvObservationParserTest, KeepsTrailingEmptyAndQuotedCommaField) {
    TemporaryCsvFile file{
        "station_code,device_code,record_time,value,note\n"
        "ST001,DV001,2026-08-08 08:00:00,,\"a,b\"\n"};

    const auto result = parse_file(file.path().string());

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_EQ(result.records.size(), 1U);
    const auto payload = Json::parse(result.records.front().payload_json);
    // 空值原样保留为空字符串，引号里的逗号不是分隔符。
    EXPECT_EQ(payload.at("value"), "");
    EXPECT_EQ(payload.at("note"), "a,b");
}

TEST(CsvObservationParserTest, RejectsRowsWithMismatchedFieldCount) {
    TemporaryCsvFile file{
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,1,2\n"
        "ST002,DV002,2026-08-08 08:01:00\n"
        "ST003,DV003,2026-08-08 08:02:00,3\n"};

    const auto result = parse_file(file.path().string());

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_EQ(result.errors.size(), 2U);
    EXPECT_NE(result.errors[0].find("line 2"), std::string::npos);
    EXPECT_NE(result.errors[1].find("line 3"), std::string::npos);
    // 多列不截断、少列不补齐，只有列数正好对上的行才发布。
    ASSERT_EQ(result.records.size(), 1U);
    EXPECT_EQ(result.records.front().station_code, "ST003");
}

TEST(CsvObservationParserTest, RejectsDuplicateAndBlankHeaderColumns) {
    TemporaryCsvFile duplicated{
        "station_code,device_code,record_time,value,value\n"};
    EXPECT_FALSE(parse_file(duplicated.path().string()).status.ok);

    TemporaryCsvFile blank{"station_code,device_code,record_time,\n"};
    EXPECT_FALSE(parse_file(blank.path().string()).status.ok);
}

TEST(CsvObservationParserTest, UnclosedQuoteFailsWholeFile) {
    TemporaryCsvFile file{
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,42\n"
        "ST002,DV002,2026-08-08 08:01:00,\"open\n"
        "ST003,DV003,2026-08-08 08:02:00,44\n"};

    const auto result = parse_file(file.path().string());

    // 引号没闭上意味着字段可能跨行，后面的行没法再按记录切分，
    // 整个文件按失败处理，而不是把第 4 行误当新记录。
    EXPECT_FALSE(result.status.ok);
    EXPECT_NE(result.status.message.find("line 3"), std::string::npos);
    EXPECT_NE(
        result.status.message.find("unclosed quote"), std::string::npos);
}

TEST(CsvObservationParserTest, ReportsQuotedFieldSyntaxErrorsAndContinues) {
    TemporaryCsvFile file{
        "station_code,device_code,record_time,value\n"
        "ST001,DV001,2026-08-08 08:00:00,bad\"quote\n"
        "ST002,DV002,2026-08-08 08:01:00,\"closed\"extra\n"
        "ST003,DV003,2026-08-08 08:02:00,46\n"};

    const auto result = parse_file(file.path().string());

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_EQ(result.errors.size(), 2U);
    EXPECT_NE(result.errors[0].find("line 2"), std::string::npos);
    EXPECT_NE(result.errors[0].find("unquoted"), std::string::npos);
    EXPECT_NE(result.errors[1].find("line 3"), std::string::npos);
    EXPECT_NE(result.errors[1].find("closing quote"), std::string::npos);
    ASSERT_EQ(result.records.size(), 1U);
    EXPECT_EQ(result.records.front().station_code, "ST003");
}

}  // namespace
