#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/result_repository.h"
#include "labbridge/server/task_run_repository.h"

#include <string>
#include <vector>

namespace labbridge::server {

struct RecordRawFileRequest {
    std::string task_run_id;
    std::string node_code;
    std::string original_name;
    std::string file_hash;
    std::string storage_path;
    long long size_bytes{0};
    std::string source_mtime;
    std::string ingest_status{"collected"};
};

struct RecordParsedRecordRequest {
    std::string task_run_id;
    std::string raw_file_id;
    labbridge::core::ParsedRecord record;
    std::string parse_status{"parsed"};
};

struct ResultCreateResult {
    labbridge::core::Status status;
    std::string id;
};

class ResultService {
public:
    ResultService(ITaskRunRepository& task_run_repository, IResultRepository& result_repository);

    ResultCreateResult record_raw_file(const RecordRawFileRequest& request);
    ResultCreateResult record_parsed_record(const RecordParsedRecordRequest& request);
    std::vector<RawFileRecord> find_raw_files(const std::string& task_run_id) const;
    std::vector<ParsedRecordRecord> find_parsed_records(const std::string& task_run_id) const;

private:
    ITaskRunRepository& task_run_repository_;
    IResultRepository& result_repository_;
};

}  // namespace labbridge::server
