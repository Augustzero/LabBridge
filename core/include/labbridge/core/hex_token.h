#pragma once

#include <string_view>

namespace labbridge::core {

// Agent 与 Server 共用的凭据格式：32 字节随机数编码为 64 位小写十六进制文本。
inline bool is_valid_hex_token(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    for (const unsigned char ch : value) {
        const bool lowercase_hex =
            (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!lowercase_hex) {
            return false;
        }
    }
    return true;
}

}  // namespace labbridge::core
