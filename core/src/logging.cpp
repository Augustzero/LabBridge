#include "labbridge/core/logging.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace labbridge::core {
namespace {

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "UNKNOWN";
}

std::string now_string() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw_time = std::chrono::system_clock::to_time_t(now);

    std::tm tm_value{};
#if defined(_WIN32)
    localtime_s(&tm_value, &raw_time);
#else
    localtime_r(&raw_time, &tm_value);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_value, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

}  // namespace

void log(LogLevel level, std::string_view component, std::string_view message) {
    std::clog << now_string() << " [" << to_string(level) << "] "
              << "[" << component << "] " << message << '\n';
}

void log_info(std::string_view component, std::string_view message) {
    log(LogLevel::Info, component, message);
}

void log_warn(std::string_view component, std::string_view message) {
    log(LogLevel::Warn, component, message);
}

void log_error(std::string_view component, std::string_view message) {
    log(LogLevel::Error, component, message);
}

}  // namespace labbridge::core

