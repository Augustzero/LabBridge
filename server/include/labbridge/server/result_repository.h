#pragma once

#include "labbridge/core/models.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace labbridge::server {

struct RawFileRecord {
    std::string id;
    std::string task_run_id;
    std::string node_code;
    std::string original_name;
    std::string file_hash;
    std::string storage_path;
    long long size_bytes{0};
    std::string source_mtime;
    std::string ingest_status{"collected"};
};

struct ParsedRecordRecord {
    std::string id;
    std::string raw_file_id;
    std::string task_run_id;
    labbridge::core::ParsedRecord record;
    std::string parse_status{"parsed"};
};

class IResultRepository {
public:
    virtual ~IResultRepository() = default;

    virtual std::string create_raw_file(RawFileRecord raw_file) = 0;
    virtual std::optional<RawFileRecord> find_raw_file(const std::string& raw_file_id) const = 0;
    virtual std::string create_parsed_record(ParsedRecordRecord parsed_record) = 0;
    virtual std::vector<ParsedRecordRecord> find_parsed_records_by_run(
        const std::string& task_run_id) const = 0;
};

class InMemoryResultRepository final : public IResultRepository {
public:
    std::string create_raw_file(RawFileRecord raw_file) override;
    std::optional<RawFileRecord> find_raw_file(const std::string& raw_file_id) const override;
    std::string create_parsed_record(ParsedRecordRecord parsed_record) override;
    std::vector<ParsedRecordRecord> find_parsed_records_by_run(
        const std::string& task_run_id) const override;

private:
    int next_raw_file_id_{1};
    int next_parsed_record_id_{1};
    std::unordered_map<std::string, RawFileRecord> raw_files_;
    std::unordered_map<std::string, ParsedRecordRecord> parsed_records_;
};

}  // namespace labbridge::server
