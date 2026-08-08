#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

namespace labbridge::core {

class CronSchedule final {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    static CronSchedule parse(std::string_view expression);
    bool matches(TimePoint time) const;
    std::optional<TimePoint> next_after(TimePoint time) const;

private:
    struct Field {
        int minimum{0};
        int maximum{0};
        std::array<bool, 60> accepted{};
    };
    std::array<Field, 5> fields_;
};

}  // namespace labbridge::core
