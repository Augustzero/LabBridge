#include "labbridge/server/postgres/result_repository.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <stdexcept>

namespace labbridge::server {
namespace {

RawFileRecord to_raw_file_record(const SqlRow& row) {
    RawFileRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.task_run_id = storage::value_or_empty(row, "task_run_id");
    record.node_code = storage::value_or_empty(row, "node_code");
    record.original_name = storage::value_or_empty(row, "original_name");
    record.file_hash = storage::value_or_empty(row, "file_hash");
    record.storage_path = storage::value_or_empty(row, "storage_path");
    record.size_bytes = storage::int64_or_zero(row, "size_bytes");
    record.source_mtime = storage::value_or_empty(row, "source_mtime");
    record.ingest_status = storage::value_or_empty(row, "ingest_status");
    return record;
}

ParsedRecordRecord to_parsed_record(const SqlRow& row) {
    ParsedRecordRecord record;
    record.id = storage::value_or_empty(row, "id");
    record.raw_file_id = storage::value_or_empty(row, "raw_file_id");
    record.task_run_id = storage::value_or_empty(row, "task_run_id");
    record.record.station_code = storage::value_or_empty(row, "station_code");
    record.record.device_code = storage::value_or_empty(row, "device_code");
    record.record.record_time = storage::value_or_empty(row, "record_time");
    record.record.payload_json = storage::value_or_empty(row, "payload_json");
    record.parse_status = storage::value_or_empty(row, "parse_status");
    return record;
}

}  // namespace

PostgresResultRepository::PostgresResultRepository(ISqlSession& session) : session_(session) {}

std::string PostgresResultRepository::create_raw_file(RawFileRecord raw_file) {
    static const std::string sql =
        "INSERT INTO raw_files "
        "(task_run_id, node_id, original_name, file_hash, storage_path, size_bytes, "
        "source_mtime, ingest_status) "
        "SELECT tr.id, n.id, $3, NULLIF($4, ''), $5, $6::bigint, "
        "NULLIF($7, '')::timestamptz, $8 "
        "FROM task_runs tr "
        "JOIN nodes n ON n.id = tr.node_id "
        "WHERE tr.id = $1::bigint AND n.node_code = $2 "
        "RETURNING id::text AS id";

    const auto row = session_.query_one(sql,
                                        {
                                            raw_file.task_run_id,
                                            raw_file.node_code,
                                            raw_file.original_name,
                                            raw_file.file_hash,
                                            raw_file.storage_path,
                                            std::to_string(raw_file.size_bytes),
                                            raw_file.source_mtime,
                                            raw_file.ingest_status,
                                        });
    if (!row.has_value()) {
        throw std::runtime_error("failed to create raw file");
    }
    return storage::value_or_empty(*row, "id");
}

std::optional<RawFileRecord> PostgresResultRepository::find_raw_file(
    const std::string& raw_file_id) const {
    static const std::string sql =
        "SELECT rf.id::text AS id, rf.task_run_id::text AS task_run_id, n.node_code, "
        "rf.original_name, COALESCE(rf.file_hash, '') AS file_hash, rf.storage_path, "
        "rf.size_bytes::text AS size_bytes, "
        "COALESCE(to_char(rf.source_mtime, 'YYYY-MM-DD HH24:MI:SS'), '') AS source_mtime, "
        "rf.ingest_status "
        "FROM raw_files rf "
        "JOIN nodes n ON n.id = rf.node_id "
        "WHERE rf.id = $1::bigint "
        "LIMIT 1";

    const auto row = session_.query_one(sql, {raw_file_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_raw_file_record(*row);
}

std::vector<RawFileRecord> PostgresResultRepository::find_raw_files_by_run(
    const std::string& task_run_id) const {
    static const std::string sql =
        "SELECT rf.id::text AS id, rf.task_run_id::text AS task_run_id, n.node_code, "
        "rf.original_name, COALESCE(rf.file_hash, '') AS file_hash, rf.storage_path, "
        "rf.size_bytes::text AS size_bytes, "
        "COALESCE(to_char(rf.source_mtime, 'YYYY-MM-DD HH24:MI:SS'), '') AS source_mtime, "
        "rf.ingest_status "
        "FROM raw_files rf "
        "JOIN nodes n ON n.id = rf.node_id "
        "WHERE rf.task_run_id = $1::bigint "
        "ORDER BY rf.id";

    std::vector<RawFileRecord> records;
    for (const auto& row : session_.query_all(sql, {task_run_id})) {
        records.push_back(to_raw_file_record(row));
    }
    return records;
}

std::string PostgresResultRepository::create_parsed_record(ParsedRecordRecord parsed_record) {
    static const std::string sql =
        "INSERT INTO parsed_records "
        "(raw_file_id, task_run_id, station_code, device_code, record_time, payload_json, parse_status) "
        "SELECT rf.id, tr.id, NULLIF($3, ''), NULLIF($4, ''), NULLIF($5, '')::timestamptz, "
        "$6::jsonb, $7 "
        "FROM raw_files rf "
        "JOIN task_runs tr ON tr.id = rf.task_run_id "
        "WHERE rf.id = $1::bigint AND tr.id = $2::bigint "
        "RETURNING id::text AS id";

    const auto row = session_.query_one(sql,
                                        {
                                            parsed_record.raw_file_id,
                                            parsed_record.task_run_id,
                                            parsed_record.record.station_code,
                                            parsed_record.record.device_code,
                                            parsed_record.record.record_time,
                                            parsed_record.record.payload_json,
                                            parsed_record.parse_status,
                                        });
    if (!row.has_value()) {
        throw std::runtime_error("failed to create parsed record");
    }
    return storage::value_or_empty(*row, "id");
}

std::optional<ParsedRecordRecord> PostgresResultRepository::find_parsed_record(
    const std::string& parsed_record_id) const {
    static const std::string sql =
        "SELECT pr.id::text AS id, COALESCE(pr.raw_file_id::text, '') AS raw_file_id, "
        "pr.task_run_id::text AS task_run_id, COALESCE(pr.station_code, '') AS station_code, "
        "COALESCE(pr.device_code, '') AS device_code, "
        "COALESCE(to_char(pr.record_time, 'YYYY-MM-DD HH24:MI:SS'), '') AS record_time, "
        "pr.payload_json::text AS payload_json, pr.parse_status "
        "FROM parsed_records pr "
        "WHERE pr.id = $1::bigint "
        "LIMIT 1";

    const auto row = session_.query_one(sql, {parsed_record_id});
    if (!row.has_value()) {
        return std::nullopt;
    }
    return to_parsed_record(*row);
}

std::vector<ParsedRecordRecord> PostgresResultRepository::find_parsed_records_by_run(
    const std::string& task_run_id) const {
    static const std::string sql =
        "SELECT pr.id::text AS id, COALESCE(pr.raw_file_id::text, '') AS raw_file_id, "
        "pr.task_run_id::text AS task_run_id, COALESCE(pr.station_code, '') AS station_code, "
        "COALESCE(pr.device_code, '') AS device_code, "
        "COALESCE(to_char(pr.record_time, 'YYYY-MM-DD HH24:MI:SS'), '') AS record_time, "
        "pr.payload_json::text AS payload_json, pr.parse_status "
        "FROM parsed_records pr "
        "WHERE pr.task_run_id = $1::bigint "
        "ORDER BY pr.id";

    std::vector<ParsedRecordRecord> records;
    for (const auto& row : session_.query_all(sql, {task_run_id})) {
        records.push_back(to_parsed_record(row));
    }
    return records;
}

}  // namespace labbridge::server
