#pragma once

#include <chrono>
#include <string>

namespace labbridge::agent {

std::string format_utc_timestamp(
    std::chrono::system_clock::time_point timestamp);

}  // namespace labbridge::agent
