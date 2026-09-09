#pragma once

#include <charconv>
#include <limits>
#include <string>
#include <system_error>

namespace labbridge::server {

// application 层共享的数据库主键字符串校验（正整数且不超过 bigint 上限）。
inline bool is_positive_id(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    unsigned long long parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} &&
           result.ptr == value.data() + value.size() &&
           parsed > 0 &&
           parsed <= static_cast<unsigned long long>(
                         std::numeric_limits<long long>::max());
}

}  // namespace labbridge::server
