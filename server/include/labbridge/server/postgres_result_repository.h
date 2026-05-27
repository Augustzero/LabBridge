#pragma once

#include "labbridge/server/result_repository.h"
#include "labbridge/server/sql_session.h"

namespace labbridge::server {

class PostgresResultRepository final : public IResultRepository {
public:
    explicit PostgresResultRepository(ISqlSession& session);

    std::string create_raw_file(RawFileRecord raw_file) override;
    std::optional<RawFileRecord> find_raw_file(const std::string& raw_file_id) const override;
    std::string create_parsed_record(ParsedRecordRecord parsed_record) override;
    std::optional<ParsedRecordRecord> find_parsed_record(
        const std::string& parsed_record_id) const override;
    std::vector<ParsedRecordRecord> find_parsed_records_by_run(
        const std::string& task_run_id) const override;

private:
    ISqlSession& session_;
};

}  // namespace labbridge::server
