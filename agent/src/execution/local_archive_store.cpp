#include "labbridge/agent/execution/local_archive_store.h"

#include <openssl/evp.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace labbridge::agent {
namespace {

std::atomic<unsigned long long> temporary_sequence{0};

std::string sha256_file(const labbridge::core::fs::path& path) {
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("failed to initialize archive SHA-256");
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

std::string format_file_time(labbridge::core::fs::file_time_type value) {
    const auto system_time = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
        value - labbridge::core::fs::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    const auto raw_time = std::chrono::system_clock::to_time_t(system_time);
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

bool is_safe_segment(const std::string& value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    for (const unsigned char character : value) {
        if (std::isalnum(character) == 0 && character != '-' &&
            character != '_' && character != '.') {
            return false;
        }
    }
    return true;
}

std::string sanitized_filename(const std::string& original_name) {
    std::string result;
    result.reserve(original_name.size());
    for (const unsigned char character : original_name) {
        if (std::isalnum(character) != 0 || character == '-' ||
            character == '_' || character == '.') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('_');
        }
    }
    if (result.empty() || result == "." || result == "..") {
        return "file";
    }
    return result;
}

}  // namespace

LocalArchiveStore::LocalArchiveStore(labbridge::core::fs::path work_dir)
    : work_dir_(labbridge::core::fs::weakly_canonical(
          labbridge::core::fs::absolute(std::move(work_dir)))) {}

LocalFileMetadata LocalArchiveStore::inspect(const CollectedItem& item) const {
    LocalFileMetadata metadata;
    const labbridge::core::fs::path input_path{item.local_path};
    const auto status = labbridge::core::fs::symlink_status(input_path);
    metadata.source_path =
        labbridge::core::fs::weakly_canonical(input_path);
    if (labbridge::core::fs::is_symlink(status) ||
        !labbridge::core::fs::is_regular_file(status)) {
        throw std::runtime_error("source is not a regular non-symlink file");
    }

    const auto size = labbridge::core::fs::file_size(metadata.source_path);
    if (size > static_cast<std::uintmax_t>(
                   std::numeric_limits<long long>::max())) {
        throw std::runtime_error("source file is too large");
    }
    metadata.size_bytes = static_cast<long long>(size);
    metadata.original_name = item.original_name;
    metadata.source_mtime =
        format_file_time(labbridge::core::fs::last_write_time(metadata.source_path));
    metadata.file_hash = sha256_file(metadata.source_path);
    metadata.fingerprint = metadata.source_path.string() + "\n" +
                           std::to_string(metadata.size_bytes) + "\n" +
                           metadata.source_mtime + "\n" + metadata.file_hash;
    return metadata;
}

ArchivedLocalFile LocalArchiveStore::archive(
    const std::string& task_id,
    const std::string& task_run_id,
    std::size_t ordinal,
    const LocalFileMetadata& source) const {
    if (!is_safe_segment(task_id) || !is_safe_segment(task_run_id)) {
        throw std::invalid_argument("task and task run IDs must be safe path segments");
    }

    const auto directory = work_dir_ / "archive" / task_id / task_run_id;
    labbridge::core::fs::create_directories(directory);
    const auto filename = std::to_string(ordinal) + "-" +
                          sanitized_filename(source.original_name);
    const auto destination = directory / filename;
    if (labbridge::core::fs::exists(destination)) {
        throw std::runtime_error("archive destination already exists");
    }

    const auto temporary =
        directory /
        (filename + ".tmp-" +
         std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed)));
        // 先写同目录临时文件并校验内容，再 rename，manifest 永远不会
        // 指向半写入的归档证据。
    try {
        labbridge::core::fs::copy_file(
            source.source_path, temporary,
            labbridge::core::fs::copy_options::none);
        const auto archive_size = labbridge::core::fs::file_size(temporary);
        const auto archive_hash = sha256_file(temporary);
        if (archive_size != static_cast<std::uintmax_t>(source.size_bytes) ||
            archive_hash != source.file_hash) {
            throw std::runtime_error("source changed while it was being archived");
        }
        labbridge::core::fs::rename(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        labbridge::core::fs::remove(temporary, ignored);
        throw;
    }

    return {source, labbridge::core::fs::weakly_canonical(destination)};
}

const labbridge::core::fs::path& LocalArchiveStore::work_dir() const noexcept {
    return work_dir_;
}

}  // namespace labbridge::agent
