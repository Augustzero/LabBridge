#include "labbridge/agent/execution/task_execution_client.h"

#include <openssl/evp.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace labbridge::agent {
namespace {

std::string sha256_hex(std::string_view value) {
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!context ||
        EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1) {
        throw std::runtime_error("failed to initialize task execution key");
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest, &digest_size) != 1) {
        throw std::runtime_error("failed to create task execution key");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

}  // namespace

std::string make_scheduled_execution_key(
    const std::string& node_code,
    const std::string& task_id,
    const std::string& scheduled_for) {
    return sha256_hex(
        "scheduled\n" + node_code + "\n" + task_id + "\n" + scheduled_for);
}

std::string make_manifest_idempotency_key(
    const std::string& node_code,
    const std::string& task_run_id) {
    return sha256_hex("manifest\n" + node_code + "\n" + task_run_id);
}

std::string make_report_idempotency_key(
    const std::string& node_code,
    const std::string& task_run_id) {
    return sha256_hex("report\n" + node_code + "\n" + task_run_id);
}

}  // namespace labbridge::agent
