#include "labbridge/server/postgres_qc_repository.h"

#include <stdexcept>

namespace labbridge::server {
namespace {

std::string bool_param(bool value) {
    return value ? "true" : "false";
}

bool from_storage_bool(const std::string& value) {
    return value == "true" || value == "t" || value == "1";
}

std::string get_or_empty(const SqlRow& row, const std::string& key) {
    const auto iter = row.find(key);
    if (iter == row.end()) {
        return {};
    }
    return iter->second;
}

QcRuleRecord to_rule_record(const SqlRow& row) {
    QcRuleRecord rule;
    rule.id = get_or_empty(row, "id");
    rule.name = get_or_empty(row, "name");
    rule.rule_type = get_or_empty(row, "rule_type");
    rule.rule_config_json = get_or_empty(row, "rule_config_json");
    rule.enabled = from_storage_bool(get_or_empty(row, "enabled"));
    return rule;
}

QcResultRecord to_result_record(const SqlRow& row) {
    QcResultRecord result;
    result.id = get_or_empty(row, "id");
    result.parsed_record_id = get_or_empty(row, "parsed_record_id");
    result.qc_rule_id = get_or_empty(row, "qc_rule_id");
    result.level = get_or_empty(row, "level");
    result.result = get_or_empty(row, "result");
    result.message = get_or_empty(row, "message");
    return result;
}

}  // namespace

PostgresQcRepository::PostgresQcRepository(ISqlSession& session) : session_(session) {}

std::string PostgresQcRepository::create_rule(QcRuleRecord rule) {
    static const std::string sql =
        "INSERT INTO qc_rules (name, rule_type, rule_config_json, enabled) "
        "VALUES ($1, $2, $3::jsonb, $4::boolean) "
        "RETURNING id::text AS id";

    const auto row = session_.query_one(sql,
                                        {
                                            rule.name,
                                            rule.rule_type,
                                            rule.rule_config_json,
                                            bool_param(rule.enabled),
                                        });
    if (!row.has_value()) {
        throw std::runtime_error("failed to create qc rule");
    }
    return get_or_empty(*row, "id");
}

std::optional<QcRuleRecord> PostgresQcRepository::find_rule(const std::string& qc_rule_id) const {
    static const std::string sql =
        "SELECT id::text AS id, name, rule_type, rule_config_json::text AS rule_config_json, "
        "CASE WHEN enabled THEN 'true' ELSE 'false' END AS enabled "
        "FROM qc_rules "
        "WHERE id = $1::bigint "
        "LIMIT 1";

    const auto row = session_.query_one(sql, {qc_rule_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_rule_record(*row);
}

std::string PostgresQcRepository::create_result(QcResultRecord result) {
    static const std::string sql =
        "INSERT INTO qc_results (parsed_record_id, qc_rule_id, level, result, message) "
        "VALUES ($1::bigint, $2::bigint, $3, $4, NULLIF($5, '')) "
        "RETURNING id::text AS id";

    const auto row = session_.query_one(sql,
                                        {
                                            result.parsed_record_id,
                                            result.qc_rule_id,
                                            result.level,
                                            result.result,
                                            result.message,
                                        });
    if (!row.has_value()) {
        throw std::runtime_error("failed to create qc result");
    }
    return get_or_empty(*row, "id");
}

std::vector<QcResultRecord> PostgresQcRepository::find_results_by_parsed_record(
    const std::string& parsed_record_id) const {
    static const std::string sql =
        "SELECT id::text AS id, parsed_record_id::text AS parsed_record_id, "
        "qc_rule_id::text AS qc_rule_id, level, result, COALESCE(message, '') AS message "
        "FROM qc_results "
        "WHERE parsed_record_id = $1::bigint "
        "ORDER BY id";

    std::vector<QcResultRecord> results;
    for (const auto& row : session_.query_all(sql, {parsed_record_id})) {
        results.push_back(to_result_record(row));
    }
    return results;
}

}  // namespace labbridge::server
