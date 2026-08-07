#include "labbridge/agent/bootstrap/process_signal_monitor.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <functional>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;

TEST(ProcessSignalMonitorTest, DispatchesStopNotificationOnOwnedThread) {
    std::promise<void> callback_called;
    auto callback_future = callback_called.get_future();

    labbridge::agent::ProcessSignalMonitor monitor{[&callback_called] {
        callback_called.set_value();
    }};
    monitor.notify_stop();

    EXPECT_EQ(callback_future.wait_for(1s), std::future_status::ready);
}

TEST(ProcessSignalMonitorTest, DestructionCancelsWaitWithoutCallingStop) {
    std::atomic<int> call_count{0};

    {
        labbridge::agent::ProcessSignalMonitor monitor{[&call_count] {
            call_count.fetch_add(1, std::memory_order_relaxed);
        }};
    }

    EXPECT_EQ(call_count.load(std::memory_order_relaxed), 0);
}

TEST(ProcessSignalMonitorTest, RejectsMissingStopCallback) {
    EXPECT_THROW(
        labbridge::agent::ProcessSignalMonitor{std::function<void()>{}},
        std::invalid_argument);
}

}  // namespace
