#include "labbridge/agent/execution/execution_request_codec.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <string_view>

namespace labbridge::agent {
namespace {

using Json = nlohmann::json;

constexpr int kCodecVersion = 1;

Json parse_payload(const std::string& text) {
    Json payload;
    try {
        payload = Json::parse(text);
    } catch (const Json::exception& error) {
        throw ExecutionCodecError(
            "invalid persisted JSON: " + std::string{error.what()});
    }

    if (!payload.is_object() ||
        payload.value("codec_version", 0) != kCodecVersion) {
        throw ExecutionCodecError("unsupported or missing codec_version");
    }
    return payload;
}

std::string required_string(const Json& object,
                            std::string_view field,
                            bool allow_empty = false) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_string() ||
        (!allow_empty && iterator->get_ref<const std::string&>().empty())) {
        throw ExecutionCodecError(
            "field '" + std::string{field} +
            "' must be a non-empty string");
    }
    return iterator->get<std::string>();
}

const Json& required_object(const Json& object, std::string_view field) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_object()) {
        throw ExecutionCodecError(
            "field '" + std::string{field} + "' must be an object");
    }
    return *iterator;
}

const Json& required_array(const Json& object, std::string_view field) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_array()) {
        throw ExecutionCodecError(
            "field '" + std::string{field} + "' must be an array");
    }
    return *iterator;
}

int required_non_negative_int(const Json& object, std::string_view field) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_number_integer()) {
        throw ExecutionCodecError(
            "field '" + std::string{field} + "' must be an integer");
    }

    const auto value = iterator->get<long long>();
    if (value < 0 || value > std::numeric_limits<int>::max()) {
        throw ExecutionCodecError(
            "field '" + std::string{field} + "' is out of range");
    }
    return static_cast<int>(value);
}

long long required_non_negative_long(const Json& object,
                                     std::string_view field) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_number_integer() ||
        iterator->get<long long>() < 0) {
        throw ExecutionCodecError(
            "field '" + std::string{field} +
            "' must be a non-negative integer");
    }
    return iterator->get<long long>();
}

bool required_boolean(const Json& object, std::string_view field) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_boolean()) {
        throw ExecutionCodecError(
            "field '" + std::string{field} + "' must be a boolean");
    }
    return iterator->get<bool>();
}

std::string encode_source_type(labbridge::core::SourceType type) {
    switch (type) {
        case labbridge::core::SourceType::LocalDirectory:
            return "local_directory";
        case labbridge::core::SourceType::Ftp:
            return "ftp";
        case labbridge::core::SourceType::Oracle:
            return "oracle";
    }
    throw ExecutionCodecError("unsupported source type");
}

labbridge::core::SourceType decode_source_type(const std::string& value) {
    if (value == "local_directory") {
        return labbridge::core::SourceType::LocalDirectory;
    }
    if (value == "ftp") {
        return labbridge::core::SourceType::Ftp;
    }
    if (value == "oracle") {
        return labbridge::core::SourceType::Oracle;
    }
    throw ExecutionCodecError("unsupported source type");
}

std::string encode_terminal_status(labbridge::core::TaskRunStatus status) {
    if (status == labbridge::core::TaskRunStatus::Succeeded) {
        return "succeeded";
    }
    if (status == labbridge::core::TaskRunStatus::Failed) {
        return "failed";
    }
    throw ExecutionCodecError("report status must be terminal");
}

labbridge::core::TaskRunStatus decode_terminal_status(
    const std::string& value) {
    if (value == "succeeded") {
        return labbridge::core::TaskRunStatus::Succeeded;
    }
    if (value == "failed") {
        return labbridge::core::TaskRunStatus::Failed;
    }
    throw ExecutionCodecError("report status must be terminal");
}

Json encode_qc_rule(const labbridge::core::QcRuleConfig& rule) {
    return {
        {"id", rule.id},
        {"rule_type", rule.rule_type},
        {"name", rule.name},
        {"config_json", rule.config_json},
    };
}

labbridge::core::QcRuleConfig decode_qc_rule(const Json& payload) {
    return {
        required_string(payload, "id"),
        required_string(payload, "rule_type"),
        required_string(payload, "name"),
        required_string(payload, "config_json", true),
    };
}

Json encode_manifest_entry(const RawFileManifestEntry& file) {
    return {
        {"original_name", file.original_name},
        {"file_hash", file.file_hash},
        {"storage_path", file.storage_path},
        {"size_bytes", file.size_bytes},
        {"source_mtime", file.source_mtime},
        {"ingest_status", file.ingest_status},
    };
}

RawFileManifestEntry decode_manifest_entry(const Json& payload) {
    return {
        required_string(payload, "original_name"),
        required_string(payload, "file_hash"),
        required_string(payload, "storage_path"),
        required_non_negative_long(payload, "size_bytes"),
        required_string(payload, "source_mtime"),
        required_string(payload, "ingest_status"),
    };
}

Json encode_qc_result(const TaskRunReportQcResult& result) {
    return {
        {"qc_rule_id", result.qc_rule_id},
        {"level", result.level},
        {"result", result.result},
        {"message", result.message},
    };
}

TaskRunReportQcResult decode_qc_result(const Json& payload) {
    return {
        required_string(payload, "qc_rule_id"),
        required_string(payload, "level"),
        required_string(payload, "result"),
        required_string(payload, "message", true),
    };
}

Json encode_parsed_record(const TaskRunReportParsedRecord& parsed) {
    Json qc_results = Json::array();
    for (const auto& result : parsed.qc_results) {
        qc_results.push_back(encode_qc_result(result));
    }

    return {
        {"raw_file_id", parsed.raw_file_id},
        {"station_code", parsed.record.station_code},
        {"device_code", parsed.record.device_code},
        {"record_time", parsed.record.record_time},
        {"payload_json", parsed.record.payload_json},
        {"parse_status", parsed.parse_status},
        {"qc_results", std::move(qc_results)},
    };
}

TaskRunReportParsedRecord decode_parsed_record(const Json& payload) {
    TaskRunReportParsedRecord parsed;
    parsed.raw_file_id = required_string(payload, "raw_file_id");
    parsed.record = {
        required_string(payload, "station_code", true),
        required_string(payload, "device_code", true),
        required_string(payload, "record_time"),
        required_string(payload, "payload_json", true),
    };
    parsed.parse_status = required_string(payload, "parse_status");

    for (const auto& result : required_array(payload, "qc_results")) {
        parsed.qc_results.push_back(decode_qc_result(result));
    }
    return parsed;
}

}  // namespace

std::string encode_task_config(const labbridge::core::TaskConfig& task) {
    Json qc_rules = Json::array();
    for (const auto& rule : task.qc_rules) {
        qc_rules.push_back(encode_qc_rule(rule));
    }

    return Json{
        {"codec_version", kCodecVersion},
        {"id", task.id},
        {"node_code", task.node_code},
        {"data_source_id", task.data_source_id},
        {"name", task.name},
        {"task_type", task.task_type},
        {"schedule_expr", task.schedule_expr},
        {"parser_type", task.parser_type},
        {"qc_profile", task.qc_profile},
        {"enabled", task.enabled},
        {"data_source",
         {
             {"id", task.data_source.id},
             {"node_code", task.data_source.node_code},
             {"type", encode_source_type(task.data_source.type)},
             {"name", task.data_source.name},
             {"config_json", task.data_source.config_json},
         }},
        {"qc_rules", std::move(qc_rules)},
    }.dump();
}

labbridge::core::TaskConfig decode_task_config(const std::string& json) {
    const auto payload = parse_payload(json);
    const auto& data_source = required_object(payload, "data_source");

    labbridge::core::TaskConfig task;
    task.id = required_string(payload, "id");
    task.node_code = required_string(payload, "node_code");
    task.data_source_id = required_string(payload, "data_source_id");
    task.name = required_string(payload, "name");
    task.task_type = required_string(payload, "task_type");
    task.schedule_expr = required_string(payload, "schedule_expr");
    task.parser_type = required_string(payload, "parser_type");
    task.qc_profile = required_string(payload, "qc_profile", true);
    task.enabled = required_boolean(payload, "enabled");
    task.data_source = {
        required_string(data_source, "id"),
        required_string(data_source, "node_code"),
        decode_source_type(required_string(data_source, "type")),
        required_string(data_source, "name"),
        required_string(data_source, "config_json", true),
    };

    for (const auto& rule : required_array(payload, "qc_rules")) {
        task.qc_rules.push_back(decode_qc_rule(rule));
    }
    return task;
}

std::string encode_start_task_run_request(const StartTaskRunRequest& request) {
    return Json{
        {"codec_version", kCodecVersion},
        {"node_code", request.node_code},
        {"task_id", request.task_id},
        {"execution_key", request.execution_key},
        {"scheduled_for", request.scheduled_for},
        {"started_at", request.started_at},
        {"trigger_type", request.trigger_type},
    }.dump();
}

StartTaskRunRequest decode_start_task_run_request(const std::string& json) {
    const auto payload = parse_payload(json);
    return {
        required_string(payload, "node_code"),
        required_string(payload, "task_id"),
        required_string(payload, "execution_key"),
        required_string(payload, "scheduled_for"),
        required_string(payload, "started_at"),
        required_string(payload, "trigger_type"),
    };
}

std::string encode_raw_file_manifest_request(
    const RawFileManifestRequest& request) {
    Json files = Json::array();
    for (const auto& file : request.files) {
        files.push_back(encode_manifest_entry(file));
    }

    return Json{
        {"codec_version", kCodecVersion},
        {"task_run_id", request.task_run_id},
        {"node_code", request.node_code},
        {"idempotency_key", request.idempotency_key},
        {"files", std::move(files)},
    }.dump();
}

RawFileManifestRequest decode_raw_file_manifest_request(
    const std::string& json) {
    const auto payload = parse_payload(json);
    RawFileManifestRequest request{
        required_string(payload, "task_run_id"),
        required_string(payload, "node_code"),
        required_string(payload, "idempotency_key"),
        {},
    };

    for (const auto& file : required_array(payload, "files")) {
        request.files.push_back(decode_manifest_entry(file));
    }
    return request;
}

std::string encode_task_run_report_request(
    const TaskRunReportRequest& request) {
    Json parsed_records = Json::array();
    for (const auto& parsed : request.parsed_records) {
        parsed_records.push_back(encode_parsed_record(parsed));
    }

    return Json{
        {"codec_version", kCodecVersion},
        {"task_run_id", request.task_run_id},
        {"node_code", request.node_code},
        {"idempotency_key", request.idempotency_key},
        {"status", encode_terminal_status(request.status)},
        {"finished_at", request.finished_at},
        {"items_total", request.items_total},
        {"items_success", request.items_success},
        {"items_failed", request.items_failed},
        {"error_summary", request.error_summary},
        {"parsed_records", std::move(parsed_records)},
    }.dump();
}

TaskRunReportRequest decode_task_run_report_request(const std::string& json) {
    const auto payload = parse_payload(json);

    TaskRunReportRequest request;
    request.task_run_id = required_string(payload, "task_run_id");
    request.node_code = required_string(payload, "node_code");
    request.idempotency_key = required_string(payload, "idempotency_key");
    request.status =
        decode_terminal_status(required_string(payload, "status"));
    request.finished_at = required_string(payload, "finished_at");
    request.items_total = required_non_negative_int(payload, "items_total");
    request.items_success =
        required_non_negative_int(payload, "items_success");
    request.items_failed =
        required_non_negative_int(payload, "items_failed");
    request.error_summary = required_string(payload, "error_summary", true);

    for (const auto& parsed : required_array(payload, "parsed_records")) {
        request.parsed_records.push_back(decode_parsed_record(parsed));
    }
    return request;
}

}  // namespace labbridge::agent
