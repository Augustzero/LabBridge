#pragma once

#include "labbridge/server/http/http_json_response.h"

#include <map>
#include <optional>
#include <string>

namespace labbridge::server {

// 控制面 HTTP 认证：持有启动时读取一次的静态凭据，四组 controller 共享。
// 管理 API 只接受管理 token；Agent API 按节点请求头找到该节点的独立密钥再比对。
class HttpAuthenticator {
public:
    struct CredentialSet {
        std::string management_token;
        // 节点编号 -> 独立密钥；允许为空映射，此时没有 Agent 可以接入。
        std::map<std::string, std::string> agent_tokens;
    };

    // 直接注入已加载的凭据；格式非法或与管理 token 重复时抛出异常。
    explicit HttpAuthenticator(CredentialSet credentials);

    // 从凭据文件构造：management_token_file 只含一个管理 token，
    // agent_tokens_file 是 agent_tokens 节点映射的 YAML 文件。
    // 文件不可读或内容非法时抛出异常，由启动流程终止进程；
    // 错误消息只包含文件路径和字段，不回显凭据内容。
    static HttpAuthenticator from_files(
        const std::string& management_token_file,
        const std::string& agent_tokens_file);

    // 管理 API 入口校验：只接受管理 token，节点密钥一律拒绝。
    // 失败时发出 401 响应并返回 false。
    bool require_management(const drogon::HttpRequestPtr& request,
                            http::ResponseCallback& callback) const;

    // Agent API 凭据校验：X-LabBridge-Node-Code 指定节点，Authorization
    // 携带该节点密钥。失败时发出 401 响应并返回 nullopt；
    // 成功时返回认证出的节点编号，供后续节点一致性检查使用。
    std::optional<std::string> require_agent_node(
        const drogon::HttpRequestPtr& request,
        http::ResponseCallback& callback) const;

    // 节点一致性检查：请求（body/路径）声明的节点必须等于认证节点，
    // 否则发出 403 响应并返回 false。在 DTO/路径解析后、业务调用前执行。
    static bool require_declared_node(const std::string& authenticated_node,
                                      const std::string& declared_node,
                                      http::ResponseCallback& callback);

private:
    // 请求携带的 Bearer 密钥与期望值做常量时间比较，避免密钥逐字节猜测。
    bool bearer_token_equals(const drogon::HttpRequestPtr& request,
                             const std::string& expected_token) const;

    CredentialSet credentials_;
};

}  // namespace labbridge::server
