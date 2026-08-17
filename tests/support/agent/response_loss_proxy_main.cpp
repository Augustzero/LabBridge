#include "support/agent/response_loss_proxy.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

unsigned short parse_port(const std::string& value) {
    const auto parsed = std::stoul(value);
    if (parsed == 0 || parsed > 65535) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }
    return static_cast<unsigned short>(parsed);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 5) {
            throw std::invalid_argument(
                "usage: phase024_response_loss_proxy <listen-port> "
                "<upstream-host> <upstream-port> <drop-target> [drop-target...]");
        }
        std::vector<std::string> targets;
        for (int index = 4; index < argc; ++index) {
            targets.emplace_back(argv[index]);
        }
        labbridge::test::support::ResponseLossProxy proxy{
            argv[2], parse_port(argv[3]), parse_port(argv[1]),
            std::move(targets)};
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::cout << "response_loss_proxy_ready port=" << proxy.port() << '\n';
        std::cout.flush();
        while (stop_requested == 0) {
            proxy.rethrow_failure();
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        proxy.request_stop();
        for (const auto& event : proxy.events()) {
            std::cout << "target=" << event.target
                      << " response_dropped="
                      << (event.response_dropped ? "true" : "false") << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
