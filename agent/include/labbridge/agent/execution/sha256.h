#pragma once

#include "labbridge/core/filesystem.h"

#include <openssl/evp.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace labbridge::agent {

// execution 模块共享的 SHA-256 摘要工具：一次性字符串摘要与文件流式摘要，
// 输出统一为小写 hex。
inline std::string sha256_hex(std::string_view value) {
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!context ||
        EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1) {
        throw std::runtime_error("failed to compute SHA-256 digest");
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest, &digest_size) != 1) {
        throw std::runtime_error("failed to compute SHA-256 digest");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

inline std::string sha256_file_hex(const labbridge::core::fs::path& path) {
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("failed to compute archive SHA-256");
    }

    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error("failed to open file for SHA-256: " + path.string());
    }

    std::array<char, 64 * 1024> buffer{};
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           input.gcount() > 0) {
        if (EVP_DigestUpdate(
                context.get(), buffer.data(),
                static_cast<std::size_t>(input.gcount())) != 1) {
            throw std::runtime_error("failed to update archive SHA-256");
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed while reading file for SHA-256");
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest, &digest_size) != 1) {
        throw std::runtime_error("failed to finish archive SHA-256");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

}  // namespace labbridge::agent
