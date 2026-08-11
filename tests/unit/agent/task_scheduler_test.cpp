#include "labbridge/agent/scheduler/task_scheduler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using SystemTimePoint = labbridge::agent::ISchedulerTimeSource::SystemTimePoint;
using SteadyTimePoint = labbridge::agent::ISchedulerTimeSource::SteadyTimePoint;

labbridge::core::TaskConfig task(std::string id,
                                 std::string schedule = "* * * * *") {
    labbridge::core::TaskConfig result;
    result.id = std::move(id);
    result.node_code = "node-022";
    result.data_source_id = "source-1";
    result.name = "scheduled task";
    result.task_type = "local_file_import";
    result.schedule_expr = std::move(schedule);
    result.parser_type = "csv_observation";
    result.data_source.id = "source-1";
    result.data_source.node_code = "node-022";
    result.data_source.config_json = R"({"root_path":"/data"})";
    result.qc_rules.push_back(
        {"rule-1", "required_fields", "required", "{}"});
    return result;
}

class ScriptedTimeSource final
    : public labbridge::agent::ISchedulerTimeSource {
public:
    SteadyTimePoint steady_now() const override { return steady; }
    SystemTimePoint system_now() const override { return system; }

    void wait_until(SteadyTimePoint deadline) override {
        steady = deadline;
        ++wait_count;
        if (on_wait) {
            on_wait(wait_count);
        }
    }

    void wake() noexcept override { ++wake_count; }

    SteadyTimePoint steady{};
    SystemTimePoint system{30s};
    std::size_t wait_count{0};
    std::size_t wake_count{0};
    std::function<void(std::size_t)> on_wait;
};

class RecordingExecutor final : public labbridge::agent::ITaskExecutor {
public:
    void execute(labbridge::agent::ScheduledTaskExecution execution) override {
        executions.push_back(std::move(execution));
        if (on_execute) {
            on_execute(executions.back());
        }
    }

    void request_stop() noexcept override {}
    void forget_task(const std::string& task_id) override {
        forgotten_task_ids.push_back(task_id);
    }
    std::vector<labbridge::agent::ScheduledTaskExecution> executions;
    std::vector<std::string> forgotten_task_ids;
    std::function<void(const labbridge::agent::ScheduledTaskExecution&)>
        on_execute;
};

TEST(TaskSchedulerTest, StartsAfterConfigTimeAndSortsNumericTaskIds) {
    ScriptedTimeSource time;
    RecordingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    auto invalid = task("bad", "1,2 * * * *");
    scheduler.replace_config(
        {task("10"), task("a"), std::move(invalid), task("2")});
    time.on_wait = [&](std::size_t wait) {
        ASSERT_EQ(wait, 1U);
        EXPECT_TRUE(executor.executions.empty());
        time.system = SystemTimePoint{60s};
    };
    executor.on_execute = [&](const auto&) {
        if (executor.executions.size() == 3U) {
            scheduler.request_stop();
        }
    };

    scheduler.run();

    ASSERT_EQ(executor.executions.size(), 3U);
    EXPECT_EQ(executor.executions[0].task.id, "2");
    EXPECT_EQ(executor.executions[1].task.id, "10");
    EXPECT_EQ(executor.executions[2].task.id, "a");
    for (const auto& execution : executor.executions) {
        EXPECT_EQ(execution.scheduled_for, SystemTimePoint{60s});
        std::cout << "dispatch task_id=" << execution.task.id
                  << " scheduled_utc_minute="
                  << std::chrono::duration_cast<std::chrono::minutes>(
                         execution.scheduled_for.time_since_epoch())
                         .count()
                  << '\n';
    }
}

TEST(TaskSchedulerTest, PreservesUnchangedProgressAndResetsChangedTasks) {
    ScriptedTimeSource time;
    RecordingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    scheduler.replace_config({task("1"), task("2"), task("removed")});
    time.on_wait = [&](std::size_t wait) {
        if (wait == 1U) {
            time.system = SystemTimePoint{40s};
            auto unchanged = task("1");
            unchanged.name = "display name changed";
            scheduler.replace_config(
                {std::move(unchanged), task("2", "2 * * * *"),
                 task("bad", "* * * *")});
        } else {
            ASSERT_EQ(wait, 2U);
            time.system = SystemTimePoint{60s};
        }
    };
    executor.on_execute = [&](const auto&) { scheduler.request_stop(); };

    scheduler.run();

    ASSERT_EQ(executor.executions.size(), 1U);
    EXPECT_EQ(executor.executions.front().task.id, "1");
    EXPECT_EQ(executor.executions.front().task.name, "display name changed");
    EXPECT_EQ(
        executor.forgotten_task_ids,
        (std::vector<std::string>{"removed"}));
}

TEST(TaskSchedulerTest, DefersRemovedTaskCleanupUntilActiveExecutionEnds) {
    ScriptedTimeSource time;
    RecordingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    scheduler.replace_config({task("1")});
    time.on_wait = [&](std::size_t) { time.system = SystemTimePoint{60s}; };
    executor.on_execute = [&](const auto&) {
        EXPECT_TRUE(executor.forgotten_task_ids.empty());
        scheduler.replace_config({});
        EXPECT_TRUE(executor.forgotten_task_ids.empty());
        scheduler.request_stop();
    };

    scheduler.run();

    EXPECT_EQ(
        executor.forgotten_task_ids,
        (std::vector<std::string>{"1"}));
}

TEST(TaskSchedulerTest, ConfigRefreshCancelsDueTaskNotYetStarted) {
    ScriptedTimeSource time;
    RecordingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    scheduler.replace_config({task("1"), task("2")});
    time.on_wait = [&](std::size_t wait) {
        if (wait == 1U) {
            time.system = SystemTimePoint{60s};
        } else {
            scheduler.request_stop();
        }
    };
    executor.on_execute = [&](const auto&) {
        ASSERT_EQ(executor.executions.size(), 1U);
        scheduler.replace_config({task("1")});
    };

    scheduler.run();

    ASSERT_EQ(executor.executions.size(), 1U);
    EXPECT_EQ(executor.executions.front().task.id, "1");
}

TEST(TaskSchedulerTest, SkipsSlotsMissedDuringLongExecution) {
    ScriptedTimeSource time;
    RecordingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    scheduler.replace_config({task("1")});
    time.on_wait = [&](std::size_t wait) {
        time.system = wait == 1U ? SystemTimePoint{60s}
                                 : SystemTimePoint{4min};
    };
    executor.on_execute = [&](const auto&) {
        if (executor.executions.size() == 1U) {
            time.system = SystemTimePoint{3min + 30s};
        } else {
            scheduler.request_stop();
        }
    };

    scheduler.run();

    ASSERT_EQ(executor.executions.size(), 2U);
    EXPECT_EQ(executor.executions[0].scheduled_for, SystemTimePoint{1min});
    EXPECT_EQ(executor.executions[1].scheduled_for, SystemTimePoint{4min});
    std::cout << "long_execution_dispatched_minutes=1,4"
              << " skipped_minutes=2,3\n";
}

TEST(TaskSchedulerTest, SkipsForwardJumpAndDoesNotRepeatAfterBackwardJump) {
    ScriptedTimeSource time;
    RecordingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    scheduler.replace_config({task("1")});
    time.on_wait = [&](std::size_t wait) {
        if (wait == 1U) {
            time.system = SystemTimePoint{3min + 10s};
        } else if (wait == 2U) {
            time.system = SystemTimePoint{4min};
        } else {
            time.system = SystemTimePoint{5min};
        }
    };
    executor.on_execute = [&](const auto&) {
        if (executor.executions.size() == 1U) {
            time.system = SystemTimePoint{2min};
            auto changed = task("1");
            changed.data_source.config_json = R"({"root_path":"/changed"})";
            scheduler.replace_config({std::move(changed)});
        } else {
            scheduler.request_stop();
        }
    };

    scheduler.run();

    ASSERT_EQ(executor.executions.size(), 2U);
    EXPECT_EQ(executor.executions[0].scheduled_for, SystemTimePoint{4min});
    EXPECT_EQ(executor.executions[1].scheduled_for, SystemTimePoint{5min});
}

TEST(TaskSchedulerTest, StopBeforeRunIsIdempotentAndDispatchesNothing) {
    ScriptedTimeSource time;
    RecordingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    scheduler.replace_config({task("1")});

    scheduler.request_stop();
    scheduler.request_stop();
    scheduler.run();

    EXPECT_TRUE(executor.executions.empty());
    EXPECT_EQ(time.wait_count, 0U);
}

TEST(TaskSchedulerTest, UnexpectedExecutorExceptionPropagates) {
    ScriptedTimeSource time;
    RecordingExecutor executor;
    labbridge::agent::TaskScheduler scheduler{executor, time};
    scheduler.replace_config({task("1")});
    time.on_wait = [&](std::size_t) { time.system = SystemTimePoint{60s}; };
    executor.on_execute = [](const auto&) {
        throw std::logic_error{"executor invariant failed"};
    };

    EXPECT_THROW(scheduler.run(), std::logic_error);
}

}  // namespace
