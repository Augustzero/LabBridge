#include "labbridge/server/http/http_authenticator.h"

#include "labbridge/core/hex_token.h"

#include <openssl/crypto.h>
#include <yaml-cpp/yaml.h>

#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace labbridge::server {
namespace {

constexpr std::string_view kAuthorizationHeader = "Authorization";
constexpr std::string_view kNodeCodeHeader = "X-LabBridge-Node-Code";
constexpr std::string_view kBearerScheme = "Bearer ";

// 头部解析或凭据比对失败统一走 401，不区分具体原因，
// 避免向调用方暴露“节点存在但密钥错”这类可试探的信息。
void reject_unauthenticated(http::ResponseCallback& callback,
                            const std::string& message) {
    auto response = http::error_response(
        drogon::k401Unauthorized, "unauthenticated", message);
    response->addHeader("WWW-Authenticate", "Bearer");
    callback(std::move(response));
}

std::string read_text_file(const std::string& path,
                           const std::string& description) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "cannot read " + description + " '" + path + "'");
    }
    return {std::istreambuf_iterator<char>{input}, {}};
}

// 单 token 文件：整个文件就是一个密钥，最多允许一个结尾换行。
std::string read_single_token_file(const std::string& path,
                                   const std::string& description) {
    auto token = read_text_file(path, description);
    if (!token.empty() && token.back() == '\n') {
        token.pop_back();
        if (!token.empty() && token.back() == '\r') {
            token.pop_back();
        }
    }
    if (!labbridge::core::is_valid_hex_token(token)) {
        throw std::runtime_error(
            description + " '" + path +
            "' must contain a single 64-character lowercase hex token");
    }
    return token;
}

// yaml-cpp 对映射里的重复键静默取最后一个值，凭据文件里这种笔误必须当成
// 配置错误启动失败，所以按原始文本再扫一遍。凭据文件只有一个顶层映射，
// 所有带缩进的 "key:" 行都是节点条目，够用且不用自己写 YAML 解析。
void reject_duplicate_node_entries(const std::string& content,
                                   const std::string& path) {
    static const std::regex entry_pattern{R"(^[ \t]+([^#\s][^:]*):)"};
    std::istringstream input{content};
    std::set<std::string> seen_nodes;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::smatch match;
        if (std::regex_search(line, match, entry_pattern) &&
            !seen_nodes.insert(match[1].str()).second) {
            throw std::runtime_error(
                "agent tokens file '" + path + "' lists node '" +
                match[1].str() + "' more than once");
        }
    }
}

std::map<std::string, std::string> read_agent_tokens_file(
    const std::string& path) {
    const auto content = read_text_file(path, "agent tokens file");
    reject_duplicate_node_entries(content, path);

    YAML::Node root;
    try {
        root = YAML::Load(content);
    } catch (const YAML::Exception&) {
        throw std::runtime_error(
            "agent tokens file '" + path + "' is not valid YAML");
    }
    const auto entries = root["agent_tokens"];
    if (!entries || !entries.IsMap()) {
        throw std::runtime_error(
            "agent tokens file '" + path +
            "' must contain an 'agent_tokens' mapping");
    }

    std::map<std::string, std::string> tokens;
    // 同一个密钥发给多个节点会让节点之间互相冒充，直接启动失败。
    std::map<std::string, std::string> node_by_token;
    for (const auto& entry : entries) {
        if (!entry.first.IsScalar() || !entry.second.IsScalar()) {
            throw std::runtime_error(
                "agent tokens file '" + path +
                "' entries must map a node code to a token string");
        }
        const auto node = entry.first.as<std::string>();
        if (node.empty()) {
            throw std::runtime_error(
                "agent tokens file '" + path +
                "' must not contain an empty node code");
        }
        const auto token = entry.second.as<std::string>();
        if (!labbridge::core::is_valid_hex_token(token)) {
            throw std::runtime_error(
                "agent tokens file '" + path + "' has an invalid token for node '" +
                node + "'; expected 64 lowercase hex characters");
        }
        const auto [owner, inserted] = node_by_token.emplace(token, node);
        if (!inserted) {
            throw std::runtime_error(
                "agent tokens file '" + path + "' assigns the same token to nodes '" +
                owner->second + "' and '" + node + "'");
        }
        tokens.emplace(node, token);
    }
    return tokens;
}

}  // namespace

HttpAuthenticator::HttpAuthenticator(CredentialSet credentials)
    : credentials_(std::move(credentials)) {
    // 直接注入路径（测试、装配）与文件路径执行同一套格式约束，
    // 管理 token 与节点密钥相同会让节点密钥具备管理权限，必须拒绝。
    if (!labbridge::core::is_valid_hex_token(credentials_.management_token)) {
        throw std::invalid_argument(
            "management token must be 64 lowercase hex characters");
    }
    for (const auto& [node, token] : credentials_.agent_tokens) {
        if (!labbridge::core::is_valid_hex_token(token)) {
            throw std::invalid_argument(
                "token for node '" + node +
                "' must be 64 lowercase hex characters");
        }
        if (token == credentials_.management_token) {
            throw std::invalid_argument(
                "token for node '" + node +
                "' must differ from the management token");
        }
    }
}

HttpAuthenticator HttpAuthenticator::from_files(
    const std::string& management_token_file,
    const std::string& agent_tokens_file) {
    CredentialSet credentials;
    credentials.management_token = read_single_token_file(
        management_token_file, "management token file");
    credentials.agent_tokens = read_agent_tokens_file(agent_tokens_file);
    return HttpAuthenticator{std::move(credentials)};
}

bool HttpAuthenticator::require_management(
    const drogon::HttpRequestPtr& request,
    http::ResponseCallback& callback) const {
    if (bearer_token_equals(request, credentials_.management_token)) {
        return true;
    }
    reject_unauthenticated(callback, "management token required");
    return false;
}

std::optional<std::string> HttpAuthenticator::require_agent_node(
    const drogon::HttpRequestPtr& request,
    http::ResponseCallback& callback) const {
    const auto node_code = request->getHeader(std::string{kNodeCodeHeader});
    const auto expected_token = credentials_.agent_tokens.find(node_code);
    // 管理 token 不在节点映射里，因此拿它调 Agent 接口同样得到 401。
    if (expected_token == credentials_.agent_tokens.end() ||
        !bearer_token_equals(request, expected_token->second)) {
        reject_unauthenticated(callback, "agent node credential required");
        return std::nullopt;
    }
    return node_code;
}

bool HttpAuthenticator::require_declared_node(
    const std::string& authenticated_node,
    const std::string& declared_node,
    http::ResponseCallback& callback) {
    if (authenticated_node == declared_node) {
        return true;
    }
    // 凭据有效但声明的节点不是自己：拒绝越权，但不进入业务处理。
    callback(http::error_response(
        drogon::k403Forbidden,
        "forbidden",
        "credential authenticates node '" + authenticated_node +
            "' but the request declares node '" + declared_node + "'"));
    return false;
}

bool HttpAuthenticator::bearer_token_equals(
    const drogon::HttpRequestPtr& request,
    const std::string& expected_token) const {
    const auto header = request->getHeader(std::string{kAuthorizationHeader});
    if (header.compare(0, kBearerScheme.size(), kBearerScheme) != 0) {
        return false;
    }
    const std::string presented_token = header.substr(kBearerScheme.size());
    if (presented_token.size() != expected_token.size()) {
        return false;
    }
    return CRYPTO_memcmp(presented_token.data(), expected_token.data(),
                         expected_token.size()) == 0;
}

}  // namespace labbridge::server
