#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/repositories/qc_repository.h"

#include <optional>
#include <string>

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

// record_result 的返回：完整记录供告警生成直接使用，不再回读。
struct QcRecordResult {
    labbridge::core::Status status;
    QcResultRecord record;
};

class QcService {
public:
    explicit QcService(IQcRepository& qc_repository);

    QcCreateResult create_rule(const CreateQcRuleRequest& request);
    // 调用方（上报链路）已在本请求内校验规则存在与 enabled、
    // parsed_record 为本事务刚创建的记录，这里只负责写入。
    QcRecordResult record_result(const RecordQcResultRequest& request);
    std::optional<QcRuleRecord> find_rule(const std::string& qc_rule_id) const;

private:
    IQcRepository& qc_repository_;
};

}  // namespace labbridge::server
