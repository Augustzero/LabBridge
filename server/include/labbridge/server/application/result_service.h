#pragma once

#include "labbridge/core/result.h"
#include "labbridge/server/repositories/result_repository.h"
#include "labbridge/server/repositories/task_run_repository.h"

#include <string>

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
    // verified_raw_file 为调用方在本请求作用域内已校验归属的原始文件，
    // 避免同一 run 的多条记录重复回读。
    ResultCreateResult record_parsed_record(
        const RecordParsedRecordRequest& request,
        const RawFileRecord& verified_raw_file);
    std::optional<RawFileRecord> find_raw_file(const std::string& raw_file_id) const;

private:
    ITaskRunRepository& task_run_repository_;
    IResultRepository& result_repository_;
};

}  // namespace labbridge::server
