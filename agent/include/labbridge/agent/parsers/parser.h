#pragma once

#include "labbridge/core/models.h"
#include "labbridge/core/result.h"

#include <string>
#include <vector>

namespace labbridge::agent {

struct RawFileContext {
    std::string task_run_id;
    std::string raw_file_id;
    std::string local_path;
};

struct ParseResult {
    labbridge::core::Status status;
    std::vector<labbridge::core::ParsedRecord> records;
    std::vector<std::string> errors;
};

class IParser {
public:
    virtual ~IParser() = default;
    virtual ParseResult parse(const RawFileContext& context) = 0;
};

}  // namespace labbridge::agent

