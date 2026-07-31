#include "support/agent/mock_http_server.h"

#include <boost/asio/error.hpp>
#include <boost/beast/core.hpp>

#include <chrono>
#include <system_error>
#include <utility>

namespace labbridge::test::support {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

MockHttpServer::MockHttpServer(std::vector<PlannedHttpResponse> responses)
    : acceptor_(context_, {tcp::v4(), 0}),
      responses_(std::move(responses)),
      thread_([this] { serve(); }) {}

MockHttpServer::~MockHttpServer() {
    stop_requested_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
}

unsigned short MockHttpServer::port() const {
    return acceptor_.local_endpoint().port();
}

void MockHttpServer::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

const std::vector<CapturedHttpRequest>& MockHttpServer::requests() const {
    return requests_;
}

void MockHttpServer::serve() {
    try {
        acceptor_.non_blocking(true);
        for (const auto& planned : responses_) {
            tcp::socket socket{context_};
            while (!stop_requested_.load()) {
                boost::system::error_code accept_error;
                acceptor_.accept(socket, accept_error);
                if (!accept_error) {
                    break;
                }
                if (accept_error == asio::error::would_block ||
                    accept_error == asio::error::try_again) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                throw boost::system::system_error{accept_error};
            }
            if (stop_requested_.load()) {
                return;
            }

            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request);
            requests_.push_back({
                request.method(),
                std::string{request.target()},
                std::string{request[http::field::content_type]},
                request.body(),
            });

            if (planned.delay.count() > 0) {
                std::this_thread::sleep_for(planned.delay);
            }

            http::response<http::string_body> response{
                planned.status,
                request.version()};
            response.set(http::field::content_type, "application/json");
            response.keep_alive(false);
            response.body() = planned.body;
            response.prepare_payload();

            boost::system::error_code ignored;
            http::write(socket, response, ignored);
            socket.shutdown(tcp::socket::shutdown_both, ignored);
        }
    } catch (...) {
        failure_ = std::current_exception();
    }
}

std::string local_server_url(unsigned short port) {
    return "http://127.0.0.1:" + std::to_string(port);
}

}  // namespace labbridge::test::support
