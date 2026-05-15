#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace labbridge::server {

using SqlParams = std::vector<std::string>;
using SqlRow = std::unordered_map<std::string, std::string>;

class ISqlSession {
public:
    virtual ~ISqlSession() = default;

    virtual void execute(const std::string& sql, const SqlParams& params) = 0;
    virtual std::optional<SqlRow> query_one(const std::string& sql, const SqlParams& params) = 0;
};

}  // namespace labbridge::server
