#include "labbridge/server/qc_repository.h"

#include <utility>

namespace labbridge::server {

std::string InMemoryQcRepository::create_rule(QcRuleRecord rule) {
    if (rule.id.empty()) {
        rule.id = std::to_string(next_qc_rule_id_++);
    }

    const auto id = rule.id;
    rules_[id] = std::move(rule);
    return id;
}

std::optional<QcRuleRecord> InMemoryQcRepository::find_rule(const std::string& qc_rule_id) const {
    const auto iter = rules_.find(qc_rule_id);
    if (iter == rules_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

std::string InMemoryQcRepository::create_result(QcResultRecord result) {
    if (result.id.empty()) {
        result.id = std::to_string(next_qc_result_id_++);
    }

    const auto id = result.id;
    results_[id] = std::move(result);
    return id;
}

std::vector<QcResultRecord> InMemoryQcRepository::find_results_by_parsed_record(
    const std::string& parsed_record_id) const {
    std::vector<QcResultRecord> results;
    for (const auto& [id, result] : results_) {
        if (result.parsed_record_id == parsed_record_id) {
            results.push_back(result);
        }
    }
    return results;
}

}  // namespace labbridge::server
