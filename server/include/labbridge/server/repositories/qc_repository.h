#pragma once

#include <optional>
#include <string>
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
    virtual std::optional<QcResultRecord> find_result(const std::string& qc_result_id) const = 0;
    virtual std::vector<QcResultRecord> find_results_by_parsed_record(
        const std::string& parsed_record_id) const = 0;
};

}  // namespace labbridge::server
