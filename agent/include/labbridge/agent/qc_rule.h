#pragma once

#include "labbridge/core/models.h"

#include <string>

namespace labbridge::agent {

enum class QcLevel {
    Pass,
    Warning,
    Failed,
};

struct QcCheckResult {
    QcLevel level{QcLevel::Pass};
    std::string rule_name;
    std::string message;
};

class IQcRule {
public:
    virtual ~IQcRule() = default;
    virtual QcCheckResult check(const labbridge::core::ParsedRecord& record) = 0;
};

}  // namespace labbridge::agent

