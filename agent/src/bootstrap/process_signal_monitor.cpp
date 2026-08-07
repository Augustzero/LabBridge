#include "labbridge/agent/bootstrap/process_signal_monitor.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <csignal>
#include <stdexcept>
#include <thread>
#include <utility>

namespace labbridge::agent {

class ProcessSignalMonitor::Impl final {
public:
    explicit Impl(std::function<void()> stop_callback)
        : stop_callback_(std::move(stop_callback)),
          signals_(context_, SIGINT, SIGTERM) {
        if (!stop_callback_) {
            throw std::invalid_argument("process stop callback must be set");
        }

        signals_.async_wait(
            [this](const boost::system::error_code& error, int) {
                if (!error) {
                    deliver_stop();
                }
            });
        thread_ = std::thread{[this] { context_.run(); }};
    }

    ~Impl() {
        boost::system::error_code ignored_error;
        signals_.cancel(ignored_error);
        context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void notify_stop() {
        boost::asio::post(context_, [this] { deliver_stop(); });
    }

private:
    void deliver_stop() {
        const bool was_delivered =
            stop_delivered_.exchange(true, std::memory_order_acq_rel);
        if (!was_delivered) {
            stop_callback_();
        }
    }

    std::function<void()> stop_callback_;
    boost::asio::io_context context_;
    boost::asio::signal_set signals_;
    std::atomic<bool> stop_delivered_{false};
    std::thread thread_;
};

ProcessSignalMonitor::ProcessSignalMonitor(
    std::function<void()> stop_callback)
    : impl_(std::make_unique<Impl>(std::move(stop_callback))) {}

ProcessSignalMonitor::~ProcessSignalMonitor() = default;

void ProcessSignalMonitor::notify_stop() {
    impl_->notify_stop();
}

}  // namespace labbridge::agent
