#pragma once

#include "labbridge/agent/qc_rule.h"

namespace labbridge::agent {

class RequiredFieldsRule final : public IQcRule {
public:
    QcCheckResult check(const labbridge::core::ParsedRecord& record) override;
};

class BasicTimestampRule final : public IQcRule {
public:
    QcCheckResult check(const labbridge::core::ParsedRecord& record) override;
};

}  // namespace labbridge::agent

