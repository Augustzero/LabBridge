#include "labbridge/agent/qc/basic_qc_rules.h"

#include <gtest/gtest.h>

#include <string>
#include <tuple>

namespace {

using labbridge::agent::QcLevel;

labbridge::core::ParsedRecord valid_record() {
    return {
        "station-a",
        "device-a",
        "2026-07-31 10:15:30",
        "{}",
    };
}

TEST(RequiredFieldsRuleTest, PassesWhenAllRequiredFieldsArePresent) {
    labbridge::agent::RequiredFieldsRule rule;

    const auto result = rule.check(valid_record());

    EXPECT_EQ(result.level, QcLevel::Pass);
    EXPECT_EQ(result.rule_name, "required_fields");
    EXPECT_FALSE(result.message.empty());
}

class MissingRequiredFieldTest
    : public testing::TestWithParam<std::tuple<std::string, std::string,
                                               std::string>> {};

TEST_P(MissingRequiredFieldTest, FailsWhenAnyRequiredFieldIsEmpty) {
    auto record = valid_record();
    record.station_code = std::get<0>(GetParam());
    record.device_code = std::get<1>(GetParam());
    record.record_time = std::get<2>(GetParam());
    labbridge::agent::RequiredFieldsRule rule;

    const auto result = rule.check(record);

    EXPECT_EQ(result.level, QcLevel::Failed);
    EXPECT_EQ(result.rule_name, "required_fields");
    EXPECT_FALSE(result.message.empty());
}

INSTANTIATE_TEST_SUITE_P(
    EachField,
    MissingRequiredFieldTest,
    testing::Values(
        std::make_tuple("", "device-a", "2026-07-31 10:15:30"),
        std::make_tuple("station-a", "", "2026-07-31 10:15:30"),
        std::make_tuple("station-a", "device-a", "")));

TEST(BasicTimestampRuleTest, PassesForExpectedTimestampShape) {
    labbridge::agent::BasicTimestampRule rule;

    const auto result = rule.check(valid_record());

    EXPECT_EQ(result.level, QcLevel::Pass);
    EXPECT_EQ(result.rule_name, "basic_timestamp_format");
    EXPECT_FALSE(result.message.empty());
}

class InvalidTimestampShapeTest
    : public testing::TestWithParam<std::string> {};

TEST_P(InvalidTimestampShapeTest, RejectsInvalidTimestampShape) {
    auto record = valid_record();
    record.record_time = GetParam();
    labbridge::agent::BasicTimestampRule rule;

    const auto result = rule.check(record);

    EXPECT_EQ(result.level, QcLevel::Failed);
    EXPECT_EQ(result.rule_name, "basic_timestamp_format");
    EXPECT_FALSE(result.message.empty());
}

INSTANTIATE_TEST_SUITE_P(
    InvalidShapes,
    InvalidTimestampShapeTest,
    testing::Values(
        "2026-07-31 10:15",
        "2026/07/31 10:15:30",
        "2026-07-31T10:15:30",
        "2026-07-31 10:15:3x"));

}  // namespace
