#include "support/agent/response_loss_proxy.h"

#include <boost/asio/connect.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <stdexcept>
#include <utility>

namespace labbridge::test::support {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

void close_socket(tcp::socket& socket) noexcept {
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

}  // namespace

ResponseLossProxy::ResponseLossProxy(
    std::string upstream_host,
    unsigned short upstream_port,
    unsigned short listen_port,
    std::vector<std::string> drop_once_targets)
    : upstream_host_(std::move(upstream_host)),
      upstream_port_(upstream_port),
      acceptor_(context_, {tcp::v4(), listen_port}) {
    if (upstream_host_.empty() || upstream_port_ == 0 ||
        drop_once_targets.empty()) {
        throw std::invalid_argument(
            "response loss proxy requires upstream and drop targets");
    }
    for (auto& target : drop_once_targets) {
        if (target.empty() ||
            !pending_drops_.emplace(std::move(target), true).second) {
            throw std::invalid_argument(
                "response loss proxy targets must be non-empty and unique");
        }
    }
    thread_ = std::thread{[this] { serve(); }};
}

ResponseLossProxy::~ResponseLossProxy() {
    request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

unsigned short ResponseLossProxy::port() const {
    return acceptor_.local_endpoint().port();
}

void ResponseLossProxy::request_stop() noexcept {
    if (stop_requested_.exchange(true)) {
        return;
    }
    asio::io_context wake_context;
    tcp::socket wake_socket{wake_context};
    boost::system::error_code ignored;
    wake_socket.connect(
        {asio::ip::address_v4::loopback(), port()}, ignored);
}

void ResponseLossProxy::rethrow_failure() const {
    std::lock_guard<std::mutex> lock{mutex_};
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

std::vector<ResponseLossEvent> ResponseLossProxy::events() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return events_;
}

void ResponseLossProxy::serve() {
    try {
        while (!stop_requested_.load()) {
            tcp::socket downstream{context_};
            acceptor_.accept(downstream);
            if (stop_requested_.load()) {
                return;
            }
            try {
                forward(std::move(downstream));
            } catch (const boost::system::system_error&) {
                // 上游离线时让当前请求表现为网络失败，代理继续等待 Server 恢复。
                close_socket(downstream);
            }
        }
    } catch (...) {
        if (!stop_requested_.load()) {
            std::lock_guard<std::mutex> lock{mutex_};
            failure_ = std::current_exception();
        }
    }
}

void ResponseLossProxy::forward(tcp::socket downstream) {
    beast::flat_buffer downstream_buffer;
    http::request<http::string_body> request;
    http::read(downstream, downstream_buffer, request);

    asio::io_context upstream_context;
    tcp::resolver resolver{upstream_context};
    tcp::socket upstream{upstream_context};
    asio::connect(
        upstream,
        resolver.resolve(upstream_host_, std::to_string(upstream_port_)));
    request.set(
        http::field::host,
        upstream_host_ + ":" + std::to_string(upstream_port_));
    request.keep_alive(false);
    http::write(upstream, request);

    beast::flat_buffer upstream_buffer;
    http::response<http::string_body> response;
    http::read(upstream, upstream_buffer, response);
    close_socket(upstream);

    const auto target = std::string{request.target()};
    bool drop = false;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        const auto found = pending_drops_.find(target);
        if (found != pending_drops_.end() && found->second) {
            found->second = false;
            drop = true;
        }
        events_.push_back({target, drop});
    }
    // 上游已完整提交响应后再断开下游，精确模拟“服务端成功、响应丢失”。
    if (!drop) {
        response.keep_alive(false);
        http::write(downstream, response);
    }
    close_socket(downstream);
}

}  // namespace labbridge::test::support
