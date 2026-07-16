#include "labbridge/agent/csv_parser.h"
#include "labbridge/server/alert_service.h"

#include <cassert>
#include <utility>

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

    std::vector<labbridge::server::QcResultRecord> find_results_by_parsed_record(
        const std::string& parsed_record_id) const override {
        return repository_.find_results_by_parsed_record(parsed_record_id);
    }

    mutable int find_result_calls{0};

private:
    labbridge::server::InMemoryQcRepository repository_;
};

void verify_csv_payload_is_valid_json_text() {
    labbridge::agent::CsvObservationParser parser;
    const auto result = parser.parse({
        "code-quality-run",
        "code-quality-file",
        "tests/fixtures/escaped_observation.csv",
    });

    assert(result.status.ok);
    assert(result.records.size() == 1);
    assert(result.records.front().payload_json ==
           R"({"note":"\"quoted\"","path":"C:\\temp"})");
}

void verify_alert_lookup_is_not_repeated() {
    labbridge::server::InMemoryTaskRunRepository task_runs;
    labbridge::server::InMemoryResultRepository results;
    CountingQcRepository qc;
    labbridge::server::InMemoryAlertRepository alerts;

    const auto task_run_id = task_runs.create({});
    auto task_run = task_runs.find_by_id(task_run_id);
    task_run->node_code = "quality-node";
    task_runs.finish(*task_run);

    labbridge::server::ParsedRecordRecord parsed_record;
    parsed_record.task_run_id = task_run_id;
    parsed_record.record.payload_json = "{}";
    const auto parsed_record_id = results.create_parsed_record(std::move(parsed_record));

    labbridge::server::QcResultRecord qc_result;
    qc_result.parsed_record_id = parsed_record_id;
    qc_result.level = "failed";
    qc_result.result = "failed";
    const auto qc_result_id = qc.create_result(std::move(qc_result));

    labbridge::server::AlertService service{task_runs, results, qc, alerts};
    const auto created = service.create_from_qc_result_if_needed({qc_result_id});
    assert(created.status.ok);
    assert(!created.id.empty());
    assert(qc.find_result_calls == 1);
}

}  // namespace

int main() {
    verify_csv_payload_is_valid_json_text();
    verify_alert_lookup_is_not_repeated();
    return 0;
}
