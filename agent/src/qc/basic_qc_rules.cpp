#include "labbridge/agent/qc/basic_qc_rules.h"

#include <cctype>

namespace labbridge::agent {
namespace {

bool is_digit_at(const std::string& value, std::size_t index) {
    return index < value.size() && std::isdigit(static_cast<unsigned char>(value[index])) != 0;
}

bool looks_like_datetime(const std::string& value) {
    if (value.size() != 19) {
        return false;
    }

    const bool separators_ok = value[4] == '-' && value[7] == '-' && value[10] == ' ' &&
                               value[13] == ':' && value[16] == ':';
    if (!separators_ok) {
        return false;
    }

    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16) {
            continue;
        }
        if (!is_digit_at(value, index)) {
            return false;
        }
    }
    return true;
}

}  // namespace

QcCheckResult RequiredFieldsRule::check(const labbridge::core::ParsedRecord& record) {
    QcCheckResult result;
    result.rule_name = "required_fields";

    if (record.station_code.empty() || record.device_code.empty() || record.record_time.empty()) {
        result.level = QcLevel::Failed;
        result.message = "station_code, device_code and record_time are required";
        return result;
    }

    result.level = QcLevel::Pass;
    result.message = "required fields are present";
    return result;
}

QcCheckResult BasicTimestampRule::check(const labbridge::core::ParsedRecord& record) {
    QcCheckResult result;
    result.rule_name = "basic_timestamp_format";

    if (!looks_like_datetime(record.record_time)) {
        result.level = QcLevel::Failed;
        result.message = "record_time must match yyyy-mm-dd hh:mm:ss";
        return result;
    }

    result.level = QcLevel::Pass;
    result.message = "record_time format is valid";
    return result;
}

}  // namespace labbridge::agent

