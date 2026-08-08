#include "labbridge/core/cron_schedule.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

namespace labbridge::core {
namespace {

constexpr std::array<int, 5> kMinimums{0, 0, 1, 1, 0};
constexpr std::array<int, 5> kMaximums{59, 23, 31, 12, 6};
constexpr std::array<std::string_view, 5> kFieldNames{
    "minute", "hour", "day-of-month", "month", "day-of-week"};
constexpr auto kSearchLimit = std::chrono::minutes{8LL * 366 * 24 * 60};

std::vector<std::string> split_fields(std::string_view expression) {
    std::vector<std::string> fields;
    std::string current;
    for (const char value : expression) {
        if (std::isspace(static_cast<unsigned char>(value)) != 0) {
            if (!current.empty()) {
                fields.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(value);
        }
    }
    if (!current.empty()) {
        fields.push_back(std::move(current));
    }
    return fields;
}

int parse_decimal(std::string_view text, std::string_view field_name) {
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](const char value) {
            return std::isdigit(static_cast<unsigned char>(value)) != 0;
        })) {
        throw std::invalid_argument("cron " + std::string{field_name} +
                                    " must be '*', '*/N', or an integer");
    }
    int result = 0;
    for (const char value : text) {
        if (result > 1000) {
            throw std::invalid_argument("cron " + std::string{field_name} +
                                        " is out of range");
        }
        result = result * 10 + (value - '0');
    }
    return result;
}

std::tm utc_tm(CronSchedule::TimePoint time) {
    const auto raw = std::chrono::system_clock::to_time_t(time);
    std::tm result{};
    if (gmtime_r(&raw, &result) == nullptr) {
        throw std::runtime_error("cannot convert system time to UTC");
    }
    return result;
}

std::chrono::system_clock::time_point next_minute(
    std::chrono::system_clock::time_point time) {
    using namespace std::chrono;
    const auto elapsed = duration_cast<seconds>(time.time_since_epoch()).count();
    const auto minute = elapsed >= 0 ? elapsed / 60 : (elapsed - 59) / 60;
    return std::chrono::system_clock::time_point{seconds{(minute + 1) * 60}};
}

}  // namespace

CronSchedule CronSchedule::parse(std::string_view expression) {
    const auto values = split_fields(expression);
    if (values.size() != 5U) {
        throw std::invalid_argument(
            "cron expression must contain exactly five fields");
    }
    CronSchedule schedule;
    for (std::size_t index = 0; index < values.size(); ++index) {
        auto& field = schedule.fields_[index];
        field.minimum = kMinimums[index];
        field.maximum = kMaximums[index];
        const std::string_view value = values[index];
        if (value == "*") {
            for (int accepted = field.minimum; accepted <= field.maximum;
                 ++accepted) {
                field.accepted[static_cast<std::size_t>(accepted)] = true;
            }
            continue;
        }
        if (value.size() > 2U && value.substr(0, 2) == "*/") {
            const int step = parse_decimal(value.substr(2), kFieldNames[index]);
            if (step <= 0 || step > field.maximum - field.minimum + 1) {
                throw std::invalid_argument(
                    "cron " + std::string{kFieldNames[index]} +
                    " step is out of range");
            }
            for (int accepted = field.minimum; accepted <= field.maximum;
                 accepted += step) {
                field.accepted[static_cast<std::size_t>(accepted)] = true;
            }
            continue;
        }
        const int accepted = parse_decimal(value, kFieldNames[index]);
        if (accepted < field.minimum || accepted > field.maximum) {
            throw std::invalid_argument("cron " +
                                        std::string{kFieldNames[index]} +
                                        " is out of range");
        }
        field.accepted[static_cast<std::size_t>(accepted)] = true;
    }
    return schedule;
}

bool CronSchedule::matches(TimePoint time) const {
    const auto value = utc_tm(time);
    const std::array<int, 5> parts{
        value.tm_min, value.tm_hour, value.tm_mday, value.tm_mon + 1,
        value.tm_wday};
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        if (!fields_[index].accepted[static_cast<std::size_t>(parts[index])]) {
            return false;
        }
    }
    return true;
}

std::optional<CronSchedule::TimePoint> CronSchedule::next_after(
    TimePoint time) const {
    auto candidate = next_minute(time);
    const auto end = candidate + kSearchLimit;
    for (; candidate <= end; candidate += std::chrono::minutes{1}) {
        if (matches(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace labbridge::core
