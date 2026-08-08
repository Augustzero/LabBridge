#include "labbridge/agent/scheduler/task_scheduler.h"

#include "labbridge/core/cron_schedule.h"
#include "labbridge/core/logging.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace labbridge::agent {
namespace {

constexpr std::string_view kComponent = "task-scheduler";
constexpr auto kClockRecheckInterval = std::chrono::seconds{60};
using SystemTimePoint = ISchedulerTimeSource::SystemTimePoint;

std::chrono::minutes utc_minute(SystemTimePoint value) {
    return std::chrono::duration_cast<std::chrono::minutes>(
        value.time_since_epoch());
}

bool same_qc_rule(const labbridge::core::QcRuleConfig& left,
                  const labbridge::core::QcRuleConfig& right) {
    return left.id == right.id && left.rule_type == right.rule_type &&
           left.config_json == right.config_json;
}

bool same_execution_config(const labbridge::core::TaskConfig& left,
                           const labbridge::core::TaskConfig& right) {
    if (left.id != right.id || left.node_code != right.node_code ||
        left.task_type != right.task_type ||
        left.schedule_expr != right.schedule_expr ||
        left.parser_type != right.parser_type ||
        left.data_source_id != right.data_source_id ||
        left.data_source.id != right.data_source.id ||
        left.data_source.node_code != right.data_source.node_code ||
        left.data_source.type != right.data_source.type ||
        left.data_source.config_json != right.data_source.config_json ||
        left.qc_rules.size() != right.qc_rules.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.qc_rules.size(); ++index) {
        if (!same_qc_rule(left.qc_rules[index], right.qc_rules[index])) {
            return false;
        }
    }
    return true;
}

std::string normalized_numeric_id(std::string_view value) {
    const auto first = value.find_first_not_of('0');
    return first == std::string_view::npos ? "0"
                                           : std::string{value.substr(first)};
}

bool task_id_less(std::string_view left, std::string_view right) {
    const auto numeric = [](std::string_view value) {
        return !value.empty() &&
               std::all_of(value.begin(), value.end(), [](const char item) {
                   return std::isdigit(static_cast<unsigned char>(item)) != 0;
               });
    };
    if (numeric(left) && numeric(right)) {
        const auto normalized_left = normalized_numeric_id(left);
        const auto normalized_right = normalized_numeric_id(right);
        if (normalized_left.size() != normalized_right.size()) {
            return normalized_left.size() < normalized_right.size();
        }
        if (normalized_left != normalized_right) {
            return normalized_left < normalized_right;
        }
    }
    return left < right;
}

}  // namespace

ISchedulerTimeSource::SteadyTimePoint
SystemSchedulerTimeSource::steady_now() const {
    return std::chrono::steady_clock::now();
}

ISchedulerTimeSource::SystemTimePoint
SystemSchedulerTimeSource::system_now() const {
    return std::chrono::system_clock::now();
}

void SystemSchedulerTimeSource::wait_until(SteadyTimePoint deadline) {
    std::unique_lock<std::mutex> lock{mutex_};
    condition_.wait_until(lock, deadline, [this] { return wake_pending_; });
    wake_pending_ = false;
}

void SystemSchedulerTimeSource::wake() noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        wake_pending_ = true;
    }
    condition_.notify_all();
}

class TaskScheduler::Impl final {
public:
    struct Entry {
        labbridge::core::TaskConfig config;
        labbridge::core::CronSchedule schedule;
        std::optional<SystemTimePoint> next_slot;
        std::optional<SystemTimePoint> last_dispatched_slot;
    };

    Impl(ITaskExecutor& executor, ISchedulerTimeSource& time_source)
        : executor{executor}, time_source{time_source} {}

    void replace_config(std::vector<labbridge::core::TaskConfig> tasks) {
        const auto applied_at = time_source.system_now();
        std::map<std::string, Entry> replacement;
        std::lock_guard<std::mutex> lock{mutex};
        for (auto& task : tasks) {
            if (!task.enabled) {
                continue;
            }
            if (task.id.empty() || replacement.count(task.id) != 0U) {
                labbridge::core::log_warn(
                    kComponent,
                    "skipping task with empty or duplicate task_id=" + task.id);
                continue;
            }

            const auto task_id = task.id;
            try {
                auto schedule =
                    labbridge::core::CronSchedule::parse(task.schedule_expr);
                std::optional<Entry> preserved;
                const auto existing = entries.find(task.id);
                if (existing != entries.end() &&
                    same_execution_config(existing->second.config, task)) {
                    preserved = existing->second;
                }
                if (preserved.has_value()) {
                    preserved->config = std::move(task);
                    replacement.emplace(task_id, std::move(*preserved));
                } else {
                    const auto prior = entries.find(task_id);
                    const auto last_dispatched =
                        prior == entries.end()
                            ? std::optional<SystemTimePoint>{}
                            : prior->second.last_dispatched_slot;
                    const auto schedule_base =
                        last_dispatched.has_value() && *last_dispatched > applied_at
                            ? *last_dispatched
                            : applied_at;
                    auto next = schedule.next_after(schedule_base);
                    replacement.emplace(
                        task_id,
                        Entry{std::move(task), std::move(schedule), next,
                              last_dispatched});
                }
            } catch (const std::invalid_argument& error) {
                labbridge::core::log_warn(
                    kComponent,
                    "skipping task_id=" + task.id + "; " + error.what());
            }
        }

        entries = std::move(replacement);
        time_source.wake();
    }

    void run() {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel)) {
            throw std::logic_error("task scheduler is already running");
        }
        struct RunningGuard {
            std::atomic<bool>& state;
            ~RunningGuard() { state.store(false, std::memory_order_release); }
        } guard{running};

        while (!stop_requested.load(std::memory_order_acquire)) {
            const auto now = time_source.system_now();
            std::vector<ScheduledTaskExecution> due;
            std::optional<SystemTimePoint> earliest;

            {
                std::lock_guard<std::mutex> lock{mutex};
                const auto current_minute = utc_minute(now);
                for (auto& [id, entry] : entries) {
                    if (entry.next_slot.has_value() &&
                        utc_minute(*entry.next_slot) < current_minute) {
                        // 系统前跳或执行阻塞造成的旧时间槽只跳过，不补跑。
                        entry.next_slot = entry.schedule.next_after(now);
                    }
                    if (entry.next_slot.has_value() &&
                        *entry.next_slot <= now &&
                        utc_minute(*entry.next_slot) == current_minute) {
                        due.push_back({entry.config, *entry.next_slot});
                    }
                    if (entry.next_slot.has_value() &&
                        (!earliest.has_value() ||
                         *entry.next_slot < *earliest)) {
                        earliest = entry.next_slot;
                    }
                }

                std::sort(due.begin(), due.end(), [](const auto& left,
                                                     const auto& right) {
                    if (left.scheduled_for != right.scheduled_for) {
                        return left.scheduled_for < right.scheduled_for;
                    }
                    return task_id_less(left.task.id, right.task.id);
                });

                // 派发前推进状态，使配置刷新和异常路径都不会重复派发。
                for (const auto& execution : due) {
                    auto& entry = entries.at(execution.task.id);
                    entry.last_dispatched_slot = execution.scheduled_for;
                    entry.next_slot =
                        entry.schedule.next_after(execution.scheduled_for);
                }
            }

            if (!due.empty()) {
                for (auto& execution : due) {
                    if (stop_requested.load(std::memory_order_acquire)) {
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock{mutex};
                        const auto current = entries.find(execution.task.id);
                        if (current == entries.end() ||
                            !same_execution_config(current->second.config,
                                                   execution.task)) {
                            continue;
                        }
                    }
                    executor.execute(std::move(execution));
                }

                const auto completed_at = time_source.system_now();
                std::lock_guard<std::mutex> lock{mutex};
                for (auto& [id, entry] : entries) {
                    if (entry.next_slot.has_value() &&
                        *entry.next_slot <= completed_at) {
                        entry.next_slot =
                            entry.schedule.next_after(completed_at);
                    }
                }
                continue;
            }

            auto delay = kClockRecheckInterval;
            if (earliest.has_value() && *earliest > now) {
                delay = std::min(
                    kClockRecheckInterval,
                    std::chrono::duration_cast<std::chrono::seconds>(
                        *earliest - now));
                delay = std::max(delay, std::chrono::seconds{1});
            }
            time_source.wait_until(
                time_source.steady_now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    delay));
        }
    }

    void request_stop() noexcept {
        const bool was_stopped =
            stop_requested.exchange(true, std::memory_order_acq_rel);
        if (!was_stopped) {
            time_source.wake();
        }
    }

    ITaskExecutor& executor;
    ISchedulerTimeSource& time_source;
    std::mutex mutex;
    std::map<std::string, Entry> entries;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
};

TaskScheduler::TaskScheduler(ITaskExecutor& executor,
                             ISchedulerTimeSource& time_source)
    : impl_(std::make_unique<Impl>(executor, time_source)) {}

TaskScheduler::~TaskScheduler() = default;

void TaskScheduler::replace_config(
    std::vector<labbridge::core::TaskConfig> tasks) {
    impl_->replace_config(std::move(tasks));
}

void TaskScheduler::run() {
    impl_->run();
}

void TaskScheduler::request_stop() noexcept {
    impl_->request_stop();
}

}  // namespace labbridge::agent
