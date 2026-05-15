#pragma once

#include <string_view>

namespace labbridge::core {

enum class LogLevel {
    Info,
    Warn,
    Error,
};

void log(LogLevel level, std::string_view component, std::string_view message);
void log_info(std::string_view component, std::string_view message);
void log_warn(std::string_view component, std::string_view message);
void log_error(std::string_view component, std::string_view message);

}  // namespace labbridge::core

