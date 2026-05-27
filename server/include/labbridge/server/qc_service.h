#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/qc_repository.h"
#include "labbridge/server/result_repository.h"

#include <string>
#include <vector>

namespace labbridge::server {

struct CreateQcRuleRequest {
    std::string name;
    std::string rule_type;
    std::string rule_config_json;
    bool enabled{true};
};

struct RecordQcResultRequest {
    std::string parsed_record_id;
    std::string qc_rule_id;
    std::string level;
    std::string result;
    std::string message;
};

struct QcCreateResult {
    labbridge::core::Status status;
    std::string id;
};

class QcService {
public:
    QcService(IResultRepository& result_repository, IQcRepository& qc_repository);

    QcCreateResult create_rule(const CreateQcRuleRequest& request);
    QcCreateResult record_result(const RecordQcResultRequest& request);
    std::vector<QcResultRecord> find_results(const std::string& parsed_record_id) const;

private:
    IResultRepository& result_repository_;
    IQcRepository& qc_repository_;
};

}  // namespace labbridge::server
