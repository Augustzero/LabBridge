#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace labbridge::test::support {

struct ResponseLossEvent {
    std::string target;
    bool response_dropped{false};
};

class ResponseLossProxy final {
public:
    ResponseLossProxy(std::string upstream_host,
                      unsigned short upstream_port,
                      unsigned short listen_port,
                      std::vector<std::string> drop_once_targets);
    ~ResponseLossProxy();

    ResponseLossProxy(const ResponseLossProxy&) = delete;
    ResponseLossProxy& operator=(const ResponseLossProxy&) = delete;

    unsigned short port() const;
    void request_stop() noexcept;
    void rethrow_failure() const;
    std::vector<ResponseLossEvent> events() const;

private:
    void serve();
    void forward(boost::asio::ip::tcp::socket downstream);

    std::string upstream_host_;
    unsigned short upstream_port_;
    boost::asio::io_context context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, bool> pending_drops_;
    std::vector<ResponseLossEvent> events_;
    std::exception_ptr failure_;
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
};

}  // namespace labbridge::test::support
