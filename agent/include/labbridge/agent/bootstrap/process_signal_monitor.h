#pragma once

#include <functional>
#include <memory>

namespace labbridge::agent {

class ProcessSignalMonitor final {
public:
    explicit ProcessSignalMonitor(std::function<void()> stop_callback);
    ~ProcessSignalMonitor();

    ProcessSignalMonitor(const ProcessSignalMonitor&) = delete;
    ProcessSignalMonitor& operator=(const ProcessSignalMonitor&) = delete;
    ProcessSignalMonitor(ProcessSignalMonitor&&) = delete;
    ProcessSignalMonitor& operator=(ProcessSignalMonitor&&) = delete;

    // 通过与进程信号相同的异步路径触发停止，供受控关闭与测试使用。
    void notify_stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace labbridge::agent
