#include "labbridge/server/postgres_agent_report_receipt_repository.h"

#include "labbridge/server/storage_mapping.h"

#include <cctype>
#include <stdexcept>
#include <utility>

namespace labbridge::server {
namespace {

std::string request_type_value(AgentReportRequestType request_type) {
    return request_type == AgentReportRequestType::RawFileManifest
               ? "raw_file_manifest"
               : "task_run_report";
}

AgentReportRequestType request_type_from_value(const std::string& value) {
    if (value == "raw_file_manifest") {
        return AgentReportRequestType::RawFileManifest;
    }
    if (value == "task_run_report") {
        return AgentReportRequestType::TaskRunReport;
    }
    throw std::runtime_error("unsupported agent report receipt request type");
}

std::vector<std::string> split_ids(const std::string& value) {
    std::vector<std::string> ids;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find(',', begin);
        ids.push_back(value.substr(
            begin,
            end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return ids;
}

std::string join_ids(const std::vector<std::string>& ids) {
    std::string joined;
    for (const auto& id : ids) {
        if (id.empty()) {
            throw std::runtime_error("agent report receipt response contains an empty id");
        }
        for (const char value : id) {
            if (std::isdigit(static_cast<unsigned char>(value)) == 0) {
                throw std::runtime_error(
                    "agent report receipt response contains an invalid id");
            }
        }
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += id;
    }
    return joined;
}

AgentReportReceipt to_receipt(const SqlRow& row) {
    AgentReportReceipt receipt;
    receipt.id = storage::value_or_empty(row, "id");
    receipt.claim.task_run_id = storage::value_or_empty(row, "task_run_id");
    receipt.claim.node_code = storage::value_or_empty(row, "node_code");
    receipt.claim.request_type =
        request_type_from_value(storage::value_or_empty(row, "request_type"));
    receipt.claim.idempotency_key =
        storage::value_or_empty(row, "idempotency_key");
    receipt.claim.request_fingerprint =
        storage::value_or_empty(row, "request_fingerprint");
    receipt.completed =
        storage::bool_value(storage::value_or_empty(row, "completed"));
    receipt.response.raw_file_ids =
        split_ids(storage::value_or_empty(row, "raw_file_ids"));
    receipt.response.parsed_record_ids =
        split_ids(storage::value_or_empty(row, "parsed_record_ids"));
    receipt.response.qc_result_ids =
        split_ids(storage::value_or_empty(row, "qc_result_ids"));
    receipt.response.alert_ids =
        split_ids(storage::value_or_empty(row, "alert_ids"));
    return receipt;
}

const std::string& select_receipt_sql() {
    static const std::string sql =
        "SELECT arr.id::text AS id, arr.task_run_id::text AS task_run_id, "
        "n.node_code, arr.request_type, arr.idempotency_key, "
        "arr.request_fingerprint, "
        "CASE WHEN arr.completed_at IS NULL THEN 'false' ELSE 'true' END AS completed, "
        "COALESCE((SELECT string_agg(item.value, ',' ORDER BY item.ordinality) "
        "FROM jsonb_array_elements_text(COALESCE(arr.response_json -> 'raw_file_ids', "
        "'[]'::jsonb)) WITH ORDINALITY AS item(value, ordinality)), '') AS raw_file_ids, "
        "COALESCE((SELECT string_agg(item.value, ',' ORDER BY item.ordinality) "
        "FROM jsonb_array_elements_text(COALESCE(arr.response_json -> 'parsed_record_ids', "
        "'[]'::jsonb)) WITH ORDINALITY AS item(value, ordinality)), '') AS parsed_record_ids, "
        "COALESCE((SELECT string_agg(item.value, ',' ORDER BY item.ordinality) "
        "FROM jsonb_array_elements_text(COALESCE(arr.response_json -> 'qc_result_ids', "
        "'[]'::jsonb)) WITH ORDINALITY AS item(value, ordinality)), '') AS qc_result_ids, "
        "COALESCE((SELECT string_agg(item.value, ',' ORDER BY item.ordinality) "
        "FROM jsonb_array_elements_text(COALESCE(arr.response_json -> 'alert_ids', "
        "'[]'::jsonb)) WITH ORDINALITY AS item(value, ordinality)), '') AS alert_ids "
        "FROM agent_report_receipts arr "
        "JOIN nodes n ON n.id = arr.node_id ";
    return sql;
}

}  // namespace

PostgresAgentReportReceiptRepository::PostgresAgentReportReceiptRepository(
    ISqlSession& session)
    : session_(session) {}

AgentReportReceiptClaimResult PostgresAgentReportReceiptRepository::claim(
    AgentReportReceiptClaim request) {
    static const std::string insert_sql =
        "INSERT INTO agent_report_receipts "
        "(node_id, task_run_id, request_type, idempotency_key, request_fingerprint) "
        "SELECT n.id, tr.id, $3, $4, $5 "
        "FROM task_runs tr "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE tr.id = $1::bigint AND n.node_code = $2 "
        "ON CONFLICT DO NOTHING "
        "RETURNING id::text AS id";

    const auto request_type = request_type_value(request.request_type);
    const auto inserted = session_.query_one(
        insert_sql,
        {
            request.task_run_id,
            request.node_code,
            request_type,
            request.idempotency_key,
            request.request_fingerprint,
        });
    if (inserted.has_value()) {
        AgentReportReceipt receipt;
        receipt.id = storage::value_or_empty(*inserted, "id");
        receipt.claim = std::move(request);
        return {AgentReportReceiptClaimState::Acquired, std::move(receipt)};
    }

    const auto same_key = session_.query_one(
        select_receipt_sql() +
            "WHERE n.node_code = $1 AND arr.request_type = $2 "
            "AND arr.idempotency_key = $3 LIMIT 1",
        {request.node_code, request_type, request.idempotency_key});
    if (same_key.has_value()) {
        auto receipt = to_receipt(*same_key);
        if (receipt.claim.request_fingerprint != request.request_fingerprint) {
            return {
                AgentReportReceiptClaimState::IdempotencyConflict,
                std::move(receipt),
            };
        }
        if (!receipt.completed) {
            throw std::runtime_error("agent report receipt is incomplete");
        }
        return {AgentReportReceiptClaimState::Replay, std::move(receipt)};
    }

    if (request.request_type == AgentReportRequestType::TaskRunReport) {
        const auto existing_report = session_.query_one(
            select_receipt_sql() +
                "WHERE arr.task_run_id = $1::bigint "
                "AND arr.request_type = 'task_run_report' LIMIT 1",
            {request.task_run_id});
        if (existing_report.has_value()) {
            return {
                AgentReportReceiptClaimState::TaskRunConflict,
                to_receipt(*existing_report),
            };
        }
    }

    throw std::runtime_error("failed to claim agent report receipt");
}

void PostgresAgentReportReceiptRepository::complete(
    const std::string& receipt_id,
    AgentReportReceiptResponse response) {
    static const std::string sql =
        "UPDATE agent_report_receipts SET "
        "response_json = jsonb_build_object("
        "'raw_file_ids', CASE WHEN $2 = '' THEN '[]'::jsonb "
        "ELSE to_jsonb(string_to_array($2, ',')) END, "
        "'parsed_record_ids', CASE WHEN $3 = '' THEN '[]'::jsonb "
        "ELSE to_jsonb(string_to_array($3, ',')) END, "
        "'qc_result_ids', CASE WHEN $4 = '' THEN '[]'::jsonb "
        "ELSE to_jsonb(string_to_array($4, ',')) END, "
        "'alert_ids', CASE WHEN $5 = '' THEN '[]'::jsonb "
        "ELSE to_jsonb(string_to_array($5, ',')) END), "
        "completed_at = now() "
        "WHERE id = $1::bigint AND completed_at IS NULL "
        "RETURNING id::text AS id";

    const auto completed = session_.query_one(
        sql,
        {
            receipt_id,
            join_ids(response.raw_file_ids),
            join_ids(response.parsed_record_ids),
            join_ids(response.qc_result_ids),
            join_ids(response.alert_ids),
        });
    if (!completed.has_value()) {
        throw std::runtime_error("failed to complete agent report receipt");
    }
}

}  // namespace labbridge::server
