#include "support/agent/mock_http_server.h"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>

#include <array>
#include <system_error>
#include <utility>

namespace labbridge::test::support {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

MockHttpServer::MockHttpServer(std::vector<PlannedHttpResponse> responses)
    : acceptor_(context_, {tcp::v4(), 0}),
      responses_(std::move(responses)),
      thread_([this] { serve(); }) {}

MockHttpServer::~MockHttpServer() {
    if (thread_.joinable()) {
        stop_requested_.store(true);
        asio::io_context wake_context;
        tcp::socket wake_socket{wake_context};
        boost::system::error_code ignored;
        wake_socket.connect(
            {asio::ip::address_v4::loopback(), port()}, ignored);
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
        for (const auto& planned : responses_) {
            tcp::socket socket{context_};
            boost::system::error_code accept_error;
            acceptor_.accept(socket, accept_error);
            if (stop_requested_.load()) {
                return;
            }
            if (accept_error) {
                throw boost::system::system_error{accept_error};
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

            if (planned.wait_for_disconnect) {
                std::array<char, 1> probe{};
                boost::system::error_code disconnect_error;
                while (!disconnect_error) {
                    socket.read_some(asio::buffer(probe), disconnect_error);
                }
                continue;
            }

            http::response<http::string_body> response{
                planned.status,
                request.version()};
            response.set(http::field::content_type, "application/json");
            response.keep_alive(false);
            response.body() = planned.body_factory
                                  ? planned.body_factory(request.body())
                                  : planned.body;
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
