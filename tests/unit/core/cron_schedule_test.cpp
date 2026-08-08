#include "labbridge/core/cron_schedule.h"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using TimePoint = labbridge::core::CronSchedule::TimePoint;

TimePoint utc_time(int year, int month, int day, int hour, int minute,
                   int second = 0) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    return std::chrono::system_clock::from_time_t(timegm(&value));
}

TEST(CronScheduleTest, MatchesSupportedFieldsInUtcWithAndDaySemantics) {
    const auto schedule =
        labbridge::core::CronSchedule::parse("*/15 23 29 2 4");

    EXPECT_TRUE(schedule.matches(utc_time(2024, 2, 29, 23, 30)));
    EXPECT_FALSE(schedule.matches(utc_time(2024, 2, 29, 23, 31)));
    EXPECT_FALSE(schedule.matches(utc_time(2020, 2, 29, 23, 30)));
}

TEST(CronScheduleTest, SupportsBoundariesAndSundayZero) {
    const auto schedule =
        labbridge::core::CronSchedule::parse("59 23 31 12 0");

    EXPECT_TRUE(schedule.matches(utc_time(2023, 12, 31, 23, 59)));
    EXPECT_FALSE(schedule.matches(utc_time(2024, 12, 31, 23, 59)));
}

TEST(CronScheduleTest, FindsStrictlyLaterMinuteIncludingLeapDay) {
    const auto schedule =
        labbridge::core::CronSchedule::parse("0 0 29 2 *");

    const auto next = schedule.next_after(utc_time(2023, 3, 1, 0, 0));

    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, utc_time(2024, 2, 29, 0, 0));
    EXPECT_EQ(*schedule.next_after(*next), utc_time(2028, 2, 29, 0, 0));
    std::cout << "next_utc=2024-02-29T00:00:00Z following_utc=2028-02-29T00:00:00Z\n";
}

TEST(CronScheduleTest, RejectsUnsupportedOrOutOfRangeSyntax) {
    const std::vector<std::string> invalid{
        "* * * *",      "* * * * * *", "*/0 * * * *",
        "60 * * * *",   "* 24 * * *",  "* * 0 * *",
        "* * * 13 *",   "* * * * 7",   "1,2 * * * *",
        "1-2 * * * *",  "MON * * * *", "? * * * *",
        "*/61 * * * *", "1x * * * *"};

    for (const auto& expression : invalid) {
        SCOPED_TRACE(expression);
        EXPECT_THROW(
            static_cast<void>(labbridge::core::CronSchedule::parse(expression)),
            std::invalid_argument);
    }
}

}  // namespace
