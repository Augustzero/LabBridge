#include "labbridge/server/application/task_run_service.h"
#include "support/server/in_memory_repositories.h"

#include <gtest/gtest.h>

#include <string>

namespace {

class TaskRunServiceTest : public testing::Test {
protected:
    std::string create_task(const std::string& node_code = "node-a",
                            bool enabled = true) {
        labbridge::server::TaskRecord task;
        task.node_code = node_code;
        task.data_source_id = "source-1";
        task.name = "collect observations";
        task.task_type = "collect_parse_qc";
        task.schedule_expr = "*/5 * * * *";
        task.parser_type = "csv_observation";
        task.qc_profile = "basic";
        task.enabled = enabled;
        return configs_.create_task(std::move(task));
    }

    labbridge::server::InMemoryConfigRepository configs_;
    labbridge::server::InMemoryTaskRunRepository task_runs_;
    labbridge::server::TaskRunService service_{configs_, task_runs_};
};

TEST_F(TaskRunServiceTest, RejectsMissingStartFields) {
    const auto missing_node = service_.start({"", "task-1", "", ""});
    const auto missing_task = service_.start({"node-a", "", "", ""});

    EXPECT_FALSE(missing_node.status.ok);
    EXPECT_FALSE(missing_task.status.ok);
    EXPECT_TRUE(missing_node.id.empty());
    EXPECT_TRUE(missing_task.id.empty());
}

TEST_F(TaskRunServiceTest, ReturnsNotFoundForMissingTask) {
    const auto result = service_.start(
        {"node-a", "missing", "2026-07-31 10:00:00", "scheduled"});

    EXPECT_FALSE(result.status.ok);
    EXPECT_EQ(result.status.code, labbridge::core::StatusCode::NotFound);
}

TEST_F(TaskRunServiceTest, RejectsDisabledAndCrossNodeTasks) {
    const auto disabled_task = create_task("node-a", false);
    const auto disabled = service_.start(
        {"node-a", disabled_task, "2026-07-31 10:00:00", "scheduled"});
    const auto other_node_task = create_task("node-b", true);
    const auto cross_node = service_.start(
        {"node-a", other_node_task, "2026-07-31 10:00:00", "scheduled"});

    EXPECT_FALSE(disabled.status.ok);
    EXPECT_EQ(disabled.status.code, labbridge::core::StatusCode::Conflict);
    EXPECT_FALSE(cross_node.status.ok);
    EXPECT_EQ(cross_node.status.code, labbridge::core::StatusCode::Conflict);
}

TEST_F(TaskRunServiceTest, StartsRunAndDefaultsEmptyTriggerToScheduled) {
    const auto task_id = create_task();

    const auto result = service_.start(
        {"node-a", task_id, "2026-07-31 10:00:00", ""});

    ASSERT_TRUE(result.status.ok) << result.status.message;
    ASSERT_FALSE(result.id.empty());
    const auto stored = service_.find_run(result.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->task_id, task_id);
    EXPECT_EQ(stored->node_code, "node-a");
    EXPECT_EQ(stored->status, labbridge::core::TaskRunStatus::Running);
    EXPECT_EQ(stored->started_at, "2026-07-31 10:00:00");
    EXPECT_EQ(stored->trigger_type, "scheduled");
}

TEST_F(TaskRunServiceTest, CreatesAndReplaysScheduledRunByStableKey) {
    const auto task_id = create_task();
    labbridge::server::StartTaskRunRequest request;
    request.node_code = "node-a";
    request.task_id = task_id;
    request.started_at = "2026-08-08T10:00:01Z";
    request.trigger_type = "scheduled";
    request.execution_key = "scheduled-key";
    request.scheduled_for = "2026-08-08T10:00:00Z";

    const auto created = service_.start(request);
    const auto replayed = service_.start(request);

    ASSERT_TRUE(created.status.ok) << created.status.message;
    ASSERT_TRUE(replayed.status.ok) << replayed.status.message;
    EXPECT_FALSE(created.replayed);
    EXPECT_TRUE(replayed.replayed);
    EXPECT_EQ(replayed.id, created.id);
    const auto stored = service_.find_run(created.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->execution_key, request.execution_key);
    EXPECT_EQ(stored->scheduled_for, request.scheduled_for);
}

TEST_F(TaskRunServiceTest, RejectsScheduledKeyReusedForDifferentIdentity) {
    const auto first_task_id = create_task();
    const auto second_task_id = create_task();
    labbridge::server::StartTaskRunRequest request;
    request.node_code = "node-a";
    request.task_id = first_task_id;
    request.started_at = "2026-08-08T10:00:01Z";
    request.trigger_type = "scheduled";
    request.execution_key = "conflicting-key";
    request.scheduled_for = "2026-08-08T10:00:00Z";
    ASSERT_TRUE(service_.start(request).status.ok);

    request.task_id = second_task_id;
    const auto different_task = service_.start(request);
    request.task_id = first_task_id;
    request.scheduled_for = "2026-08-08T10:05:00Z";
    const auto different_slot = service_.start(request);

    EXPECT_FALSE(different_task.status.ok);
    EXPECT_EQ(
        different_task.status.code,
        labbridge::core::StatusCode::Conflict);
    EXPECT_FALSE(different_slot.status.ok);
    EXPECT_EQ(
        different_slot.status.code,
        labbridge::core::StatusCode::Conflict);
}

TEST_F(TaskRunServiceTest, ValidatesScheduledStartContract) {
    const auto task_id = create_task();
    labbridge::server::StartTaskRunRequest request;
    request.node_code = "node-a";
    request.task_id = task_id;
    request.started_at = "2026-08-08T10:00:01Z";
    request.trigger_type = "scheduled";
    request.execution_key = "key";
    request.scheduled_for = "2026-02-29T10:00:00Z";

    EXPECT_FALSE(service_.start(request).status.ok);
    request.scheduled_for = "2026-08-08T10:00:00Z";
    request.started_at = "2026-08-08 10:00:01";
    EXPECT_FALSE(service_.start(request).status.ok);
    request.started_at = "2026-08-08T10:00:01Z";
    request.trigger_type = "manual";
    EXPECT_FALSE(service_.start(request).status.ok);
    request.trigger_type = "scheduled";
    request.execution_key.assign(129, 'x');
    EXPECT_FALSE(service_.start(request).status.ok);
}

TEST_F(TaskRunServiceTest, RejectsInvalidFinishRequests) {
    const auto missing_id = service_.finish({});
    labbridge::server::FinishTaskRunRequest invalid_status;
    invalid_status.task_run_id = "run-1";
    invalid_status.status = labbridge::core::TaskRunStatus::Running;
    const auto invalid = service_.finish(invalid_status);

    EXPECT_FALSE(missing_id.ok);
    EXPECT_FALSE(invalid.ok);
}

TEST_F(TaskRunServiceTest, ReturnsNotFoundForMissingRun) {
    labbridge::server::FinishTaskRunRequest request;
    request.task_run_id = "missing";
    request.status = labbridge::core::TaskRunStatus::Succeeded;

    const auto status = service_.finish(request);

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.code, labbridge::core::StatusCode::NotFound);
}

TEST_F(TaskRunServiceTest, FinishesRunWithReportedSummary) {
    const auto task_id = create_task();
    const auto started = service_.start(
        {"node-a", task_id, "2026-07-31 10:00:00", "manual"});
    ASSERT_TRUE(started.status.ok) << started.status.message;
    labbridge::server::FinishTaskRunRequest request;
    request.task_run_id = started.id;
    request.status = labbridge::core::TaskRunStatus::Failed;
    request.finished_at = "2026-07-31 10:01:00";
    request.items_total = 5;
    request.items_success = 4;
    request.items_failed = 1;
    request.error_summary = "one item failed";

    const auto status = service_.finish(request);

    ASSERT_TRUE(status.ok) << status.message;
    const auto stored = service_.find_run(started.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->status, labbridge::core::TaskRunStatus::Failed);
    EXPECT_EQ(stored->finished_at, request.finished_at);
    EXPECT_EQ(stored->items_total, 5);
    EXPECT_EQ(stored->items_success, 4);
    EXPECT_EQ(stored->items_failed, 1);
    EXPECT_EQ(stored->error_summary, request.error_summary);
}

TEST_F(TaskRunServiceTest, EmptyRunIdIsNotQueried) {
    EXPECT_FALSE(service_.find_run("").has_value());
}

}  // namespace
