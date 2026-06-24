#pragma once

#include "labbridge/server/qc_repository.h"
#include "labbridge/server/sql_session.h"

namespace labbridge::server {

class PostgresQcRepository final : public IQcRepository {
public:
    explicit PostgresQcRepository(ISqlSession& session);

    std::string create_rule(QcRuleRecord rule) override;
    std::optional<QcRuleRecord> find_rule(const std::string& qc_rule_id) const override;
    std::string create_result(QcResultRecord result) override;
    std::optional<QcResultRecord> find_result(const std::string& qc_result_id) const override;
    std::vector<QcResultRecord> find_results_by_parsed_record(
        const std::string& parsed_record_id) const override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
