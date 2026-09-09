#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace labbridge::core {

// 全系统统一的 UTC 秒级时间戳格式（YYYY-MM-DDTHH:MM:SSZ）。
inline std::string format_utc_timestamp(
    std::chrono::system_clock::time_point timestamp) {
    const auto raw_time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw_time);
#else
    gmtime_r(&raw_time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

}  // namespace labbridge::core
