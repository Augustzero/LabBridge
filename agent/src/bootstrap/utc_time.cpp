#include "labbridge/agent/bootstrap/utc_time.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace labbridge::agent {

std::string format_utc_timestamp(
    std::chrono::system_clock::time_point timestamp) {
    const auto raw_time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc_time{};
#if defined(_WIN32)
    gmtime_s(&utc_time, &raw_time);
#else
    gmtime_r(&raw_time, &utc_time);
#endif

    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

}  // namespace labbridge::agent
