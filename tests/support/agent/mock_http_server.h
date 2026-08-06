#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace labbridge::test::support {

namespace http = boost::beast::http;

struct PlannedHttpResponse {
    http::status status;
    std::string body;
    bool wait_for_disconnect{false};
    std::function<std::string(std::string_view)> body_factory;
};

struct CapturedHttpRequest {
    http::verb method;
    std::string target;
    std::string content_type;
    std::string body;
};

class MockHttpServer final {
public:
    explicit MockHttpServer(std::vector<PlannedHttpResponse> responses);
    ~MockHttpServer();

    MockHttpServer(const MockHttpServer&) = delete;
    MockHttpServer& operator=(const MockHttpServer&) = delete;

    unsigned short port() const;
    void join();
    const std::vector<CapturedHttpRequest>& requests() const;

private:
    void serve();

    boost::asio::io_context context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<PlannedHttpResponse> responses_;
    std::vector<CapturedHttpRequest> requests_;
    std::exception_ptr failure_;
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
};

std::string local_server_url(unsigned short port);

}  // namespace labbridge::test::support
