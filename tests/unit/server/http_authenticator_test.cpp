#include "labbridge/server/http/http_authenticator.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using labbridge::server::HttpAuthenticator;
using ResponseCallback = labbridge::server::http::ResponseCallback;

// 64 位小写十六进制的合法凭据。
const std::string kManagementToken(64, '1');
const std::string kAgentToken(64, 'a');

class HttpAuthenticatorTest : public testing::Test {
protected:
    void SetUp() override {
        directory_ = fs::temp_directory_path() /
                     ("labbridge-http-auth-" +
                      std::to_string(static_cast<long>(::getpid())));
        fs::create_directories(directory_);
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }

    void write_file(const fs::path& path, const std::string& content) const {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(output) << "cannot write " << path;
        output << content;
    }

    std::string management_file(const std::string& content) const {
        const auto path = directory_ / "management.token";
        write_file(path, content);
        return path.string();
    }

    std::string agent_tokens_file(const std::string& content) const {
        const auto path = directory_ / "agent-tokens.yaml";
        write_file(path, content);
        return path.string();
    }

    // 加载失败时 from_files 抛异常；EXPECT_STATEMENT 辅助里显式吞掉返回值。
    void load_from_files(const std::string& management_path,
                         const std::string& agent_tokens_path) const {
        static_cast<void>(
            HttpAuthenticator::from_files(management_path, agent_tokens_path));
    }

    fs::path directory_;
};

// 构造一个捕获响应的回调；require_* 的 callback 参数是非 const 引用，
// 调用方需先落到局部变量再传入。
ResponseCallback capture_response(drogon::HttpResponsePtr& response) {
    return [&response](const drogon::HttpResponsePtr& current) {
        response = current;
    };
}

TEST_F(HttpAuthenticatorTest, LoadsTokensFromFilesAndAuthenticatesBothSides) {
    const auto authenticator = HttpAuthenticator::from_files(
        management_file(kManagementToken + "\n"),
        agent_tokens_file(
            "agent_tokens:\n"
            "  station-001: \"" + kAgentToken + "\"\n"));

    // 管理 token 可以进管理接口（回调只在失败时触发，response 为空即通过）。
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->addHeader("Authorization", "Bearer " + kManagementToken);
    drogon::HttpResponsePtr response;
    auto management_callback = capture_response(response);
    static_cast<void>(
        authenticator.require_management(request, management_callback));
    EXPECT_EQ(response, nullptr);

    // 节点密钥可以认证出节点身份。
    auto agent_request = drogon::HttpRequest::newHttpRequest();
    agent_request->setMethod(drogon::Get);
    agent_request->addHeader("Authorization", "Bearer " + kAgentToken);
    agent_request->addHeader("X-LabBridge-Node-Code", "station-001");
    bool rejected = false;
    ResponseCallback agent_callback =
        [&rejected](const drogon::HttpResponsePtr&) { rejected = true; };
    const auto authenticated_node =
        authenticator.require_agent_node(agent_request, agent_callback);
    EXPECT_FALSE(rejected);
    ASSERT_TRUE(authenticated_node.has_value());
    EXPECT_EQ(*authenticated_node, "station-001");
}

TEST_F(HttpAuthenticatorTest, RejectsUnreadableOrInvalidManagementTokenFile) {
    EXPECT_THROW(
        (load_from_files((directory_ / "missing.token").string(),
                         agent_tokens_file("agent_tokens: {}\n"))),
        std::runtime_error);

    // 非十六进制、长度不对、多行内容都拒绝；错误只描述文件与格式。
    const std::vector<std::string> invalid_contents = {
        "short-token\n",
        std::string(64, 'g') + "\n",
        kManagementToken + "\nextra\n",
    };
    for (const auto& content : invalid_contents) {
        SCOPED_TRACE(content);
        try {
            load_from_files(management_file(content),
                            agent_tokens_file("agent_tokens: {}\n"));
            FAIL() << "expected invalid management token file";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string{error.what()}.find(
                          "must contain a single 64-character lowercase hex token"),
                      std::string::npos);
        }
    }
}

TEST_F(HttpAuthenticatorTest, RejectsInvalidAgentTokensFile) {
    const auto management = management_file(kManagementToken);

    // 文件不存在 / 不是 YAML / 缺少 agent_tokens 映射。
    EXPECT_THROW(
        (load_from_files(management, (directory_ / "missing.yaml").string())),
        std::runtime_error);
    EXPECT_THROW(
        (load_from_files(management, agent_tokens_file("\t-not-yaml-[\n"))),
        std::runtime_error);
    const std::vector<std::string> missing_mappings = {
        "credentials: {}\n",
        "agent_tokens:\n",
        "agent_tokens: []\n",
    };
    for (const auto& content : missing_mappings) {
        SCOPED_TRACE(content);
        EXPECT_THROW((load_from_files(management, agent_tokens_file(content))),
                     std::runtime_error);
    }

    // 空 mapping 合法：没有 Agent 可以接入，但管理接口照常工作。
    const auto empty_mapping =
        HttpAuthenticator::from_files(management,
                                      agent_tokens_file("agent_tokens: {}\n"));
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->addHeader("Authorization", "Bearer " + kManagementToken);
    request->addHeader("X-LabBridge-Node-Code", "station-001");
    drogon::HttpResponsePtr rejected;
    auto agent_callback = capture_response(rejected);
    static_cast<void>(
        empty_mapping.require_agent_node(request, agent_callback));
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->statusCode(), drogon::k401Unauthorized);
    EXPECT_FALSE(rejected->getHeader("WWW-Authenticate").empty());
}

TEST_F(HttpAuthenticatorTest, RejectsInvalidAgentTokenEntries) {
    const auto management = management_file(kManagementToken);
    const std::vector<std::string> invalid_files = {
        "agent_tokens:\n  station-001: \"short\"\n",
        "agent_tokens:\n  \"\": \"" + kAgentToken + "\"\n",
    };
    for (const auto& content : invalid_files) {
        SCOPED_TRACE(content);
        EXPECT_THROW((load_from_files(management, agent_tokens_file(content))),
                     std::runtime_error);
    }
}

TEST_F(HttpAuthenticatorTest, RejectsDuplicateNodeOrTokenEntries) {
    const auto management = management_file(kManagementToken);

    // yaml-cpp 对重复键静默取后者，这里必须按配置错误启动失败。
    try {
        load_from_files(
            management,
            agent_tokens_file(
                "agent_tokens:\n"
                "  station-001: \"" + kAgentToken + "\"\n"
                "  station-001: \"" + kManagementToken + "\"\n"));
        FAIL() << "expected duplicate node entry";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string{error.what()}.find("more than once"),
                  std::string::npos);
    }

    // 两个节点共用同一密钥会让节点互相冒充，同样拒绝。
    EXPECT_THROW(
        (load_from_files(management,
                         agent_tokens_file(
                             "agent_tokens:\n"
                             "  station-001: \"" + kAgentToken + "\"\n"
                             "  station-002: \"" + kAgentToken + "\"\n"))),
        std::runtime_error);
}

TEST_F(HttpAuthenticatorTest, RejectsManagementTokenEqualToAgentToken) {
    EXPECT_THROW(
        (load_from_files(management_file(kAgentToken),
                         agent_tokens_file("agent_tokens:\n"
                                           "  station-001: \"" + kAgentToken +
                                           "\"\n"))),
        std::invalid_argument);
}

TEST(HttpAuthenticatorCredentialValidation, ValidatesDirectCredentialSet) {
    // 构造参数带逗号，用括号包住被测表达式。
    EXPECT_THROW(
        (static_cast<void>(HttpAuthenticator{
            {std::string(32, 'm'), {}}})),
        std::invalid_argument);
    EXPECT_THROW(
        (static_cast<void>(HttpAuthenticator{
            {std::string(64, '1'), {{"station-001", "not-hex"}}}})),
        std::invalid_argument);
    EXPECT_THROW(
        (static_cast<void>(HttpAuthenticator{
            {std::string(64, '1'), {{"station-001", std::string(64, '1')}}}})),
        std::invalid_argument);
}

}  // namespace
