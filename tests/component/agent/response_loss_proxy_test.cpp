#include "support/agent/mock_http_server.h"
#include "support/agent/response_loss_proxy.h"
#include <boost/asio/connect.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
std::string post(unsigned short port, const std::string& target) {
    asio::io_context context;
    tcp::resolver resolver{context};
    beast::tcp_stream stream{context};
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(port)));
    http::request<http::string_body> request{http::verb::post, target, 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.body() = R"({"idempotency_key":"stable-key"})";
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code error;
    http::read(stream, buffer, response, error);
    if (error) {
        return "network_error";
    }
    return response.body();
}
TEST(ResponseLossProxyTest, DropsOnlyFirstCompletedUpstreamResponse) {
    labbridge::test::support::MockHttpServer upstream{{
        {http::status::created, R"({"replayed":false})"},
        {http::status::ok, R"({"replayed":true})"},
    }};
    labbridge::test::support::ResponseLossProxy proxy{
        "127.0.0.1", upstream.port(), 0,
        {"/api/v1/raw-files/manifest"}};
    EXPECT_EQ(post(proxy.port(), "/api/v1/raw-files/manifest"), "network_error");
    EXPECT_EQ(post(proxy.port(), "/api/v1/raw-files/manifest"),
              R"({"replayed":true})");
    upstream.join();
    proxy.rethrow_failure();
    const auto events = proxy.events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_TRUE(events[0].response_dropped);
    EXPECT_FALSE(events[1].response_dropped);
    ASSERT_EQ(upstream.requests().size(), 2U);
    EXPECT_EQ(upstream.requests()[0].body, upstream.requests()[1].body);
    std::cout << "response_loss upstream_commits=2 first_response=dropped "
                 "replay_body=identical replayed=true\n";
}
}  // namespace
