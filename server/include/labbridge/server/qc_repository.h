#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace labbridge::server {

struct QcRuleRecord {
    std::string id;
    std::string name;
    std::string rule_type;
    std::string rule_config_json;
    bool enabled{true};
};

struct QcResultRecord {
    std::string id;
    std::string parsed_record_id;
    std::string qc_rule_id;
    std::string level;
    std::string result;
    std::string message;
};

class IQcRepository {
public:
    virtual ~IQcRepository() = default;

    virtual std::string create_rule(QcRuleRecord rule) = 0;
    virtual std::optional<QcRuleRecord> find_rule(const std::string& qc_rule_id) const = 0;
    virtual std::string create_result(QcResultRecord result) = 0;
    virtual std::vector<QcResultRecord> find_results_by_parsed_record(
        const std::string& parsed_record_id) const = 0;
};

class InMemoryQcRepository final : public IQcRepository {
public:
    std::string create_rule(QcRuleRecord rule) override;
    std::optional<QcRuleRecord> find_rule(const std::string& qc_rule_id) const override;
    std::string create_result(QcResultRecord result) override;
    std::vector<QcResultRecord> find_results_by_parsed_record(
        const std::string& parsed_record_id) const override;

private:
    int next_qc_rule_id_{1};
    int next_qc_result_id_{1};
    std::unordered_map<std::string, QcRuleRecord> rules_;
    std::unordered_map<std::string, QcResultRecord> results_;
};

}  // namespace labbridge::server
