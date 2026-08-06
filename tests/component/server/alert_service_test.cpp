#include "labbridge/server/application/alert_service.h"
#include "support/server/in_memory_repositories.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class CountingQcRepository final : public labbridge::server::IQcRepository {
public:
    std::string create_rule(labbridge::server::QcRuleRecord rule) override {
        return repository_.create_rule(std::move(rule));
    }

    std::optional<labbridge::server::QcRuleRecord> find_rule(
        const std::string& qc_rule_id) const override {
        return repository_.find_rule(qc_rule_id);
    }

    std::string create_result(labbridge::server::QcResultRecord result) override {
        return repository_.create_result(std::move(result));
    }

    std::optional<labbridge::server::QcResultRecord> find_result(
        const std::string& qc_result_id) const override {
        ++find_result_calls;
        return repository_.find_result(qc_result_id);
    }

    std::vector<labbridge::server::QcResultRecord>
    find_results_by_parsed_record(
        const std::string& parsed_record_id) const override {
        return repository_.find_results_by_parsed_record(parsed_record_id);
    }

    mutable int find_result_calls{0};

private:
    labbridge::server::InMemoryQcRepository repository_;
};

TEST(AlertServiceTest, LooksUpQcResultOnlyOnceWhenCreatingNeededAlert) {
    labbridge::server::InMemoryTaskRunRepository task_runs;
    labbridge::server::InMemoryResultRepository results;
    CountingQcRepository qc;
    labbridge::server::InMemoryAlertRepository alerts;

    const auto task_run_id = task_runs.create({});
    auto task_run = task_runs.find_by_id(task_run_id);
    ASSERT_TRUE(task_run.has_value());
    task_run->node_code = "quality-node";
    task_runs.finish(*task_run);

    labbridge::server::ParsedRecordRecord parsed_record;
    parsed_record.task_run_id = task_run_id;
    parsed_record.record.payload_json = "{}";
    const auto parsed_record_id =
        results.create_parsed_record(std::move(parsed_record));

    labbridge::server::QcResultRecord qc_result;
    qc_result.parsed_record_id = parsed_record_id;
    qc_result.level = "failed";
    qc_result.result = "failed";
    const auto qc_result_id = qc.create_result(std::move(qc_result));
    labbridge::server::AlertService service{task_runs, results, qc, alerts};

    const auto created =
        service.create_from_qc_result_if_needed({qc_result_id});

    EXPECT_TRUE(created.status.ok) << created.status.message;
    EXPECT_FALSE(created.id.empty());
    EXPECT_EQ(qc.find_result_calls, 1);
}

}  // namespace
