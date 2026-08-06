#include "labbridge/server/application/qc_service.h"

#include <utility>

namespace labbridge::server {

QcService::QcService(IResultRepository& result_repository, IQcRepository& qc_repository)
    : result_repository_(result_repository), qc_repository_(qc_repository) {}

QcCreateResult QcService::create_rule(const CreateQcRuleRequest& request) {
    if (request.name.empty()) {
        return {labbridge::core::Status::failure("qc rule name is required"), {}};
    }
    if (request.rule_type.empty()) {
        return {labbridge::core::Status::failure("rule_type is required"), {}};
    }
    if (request.rule_config_json.empty()) {
        return {labbridge::core::Status::failure("rule_config_json is required"), {}};
    }

    QcRuleRecord rule;
    rule.name = request.name;
    rule.rule_type = request.rule_type;
    rule.rule_config_json = request.rule_config_json;
    rule.enabled = request.enabled;

    const auto id = qc_repository_.create_rule(std::move(rule));
    return {labbridge::core::Status::success(), id};
}

QcCreateResult QcService::record_result(const RecordQcResultRequest& request) {
    if (request.parsed_record_id.empty()) {
        return {labbridge::core::Status::failure("parsed_record_id is required"), {}};
    }
    if (request.qc_rule_id.empty()) {
        return {labbridge::core::Status::failure("qc_rule_id is required"), {}};
    }
    if (request.level.empty()) {
        return {labbridge::core::Status::failure("level is required"), {}};
    }
    if (request.result.empty()) {
        return {labbridge::core::Status::failure("result is required"), {}};
    }

    if (!result_repository_.find_parsed_record(request.parsed_record_id).has_value()) {
        return {labbridge::core::Status::failure(
                    labbridge::core::StatusCode::NotFound,
                    "parsed record is not found"),
                {}};
    }

    const auto rule = qc_repository_.find_rule(request.qc_rule_id);
    if (!rule.has_value()) {
        return {labbridge::core::Status::failure(labbridge::core::StatusCode::NotFound, "qc rule is not found"), {}};
    }
    if (!rule->enabled) {
        return {labbridge::core::Status::failure(labbridge::core::StatusCode::Conflict, "qc rule is disabled"), {}};
    }

    QcResultRecord result;
    result.parsed_record_id = request.parsed_record_id;
    result.qc_rule_id = request.qc_rule_id;
    result.level = request.level;
    result.result = request.result;
    result.message = request.message;

    const auto id = qc_repository_.create_result(std::move(result));
    return {labbridge::core::Status::success(), id};
}

std::vector<QcResultRecord> QcService::find_results(const std::string& parsed_record_id) const {
    if (parsed_record_id.empty()) {
        return {};
    }
    return qc_repository_.find_results_by_parsed_record(parsed_record_id);
}

}  // namespace labbridge::server
