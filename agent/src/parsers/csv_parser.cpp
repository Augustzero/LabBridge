#include "labbridge/agent/parsers/csv_parser.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace labbridge::agent {
namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

// nlohmann dump 已含首尾引号，去掉后按字段拼接。
std::string json_string_value(const std::string& value) {
    const auto dumped = nlohmann::json(value).dump();
    return dumped.substr(1, dumped.size() - 2);
}

std::string make_payload_json(const std::vector<std::string>& header, const std::vector<std::string>& row) {
    // 键序必须保持 header 顺序，不能用 json 对象整体 dump（键会被字典序重排）。
    std::ostringstream payload;
    payload << "{";
    bool first = true;
    for (std::size_t index = 3; index < header.size(); ++index) {
        if (!first) {
            payload << ",";
        }
        payload << "\"" << json_string_value(header[index]) << "\":\""
                << json_string_value(row[index]) << "\"";
        first = false;
    }
    payload << "}";
    return payload.str();
}

struct CsvLineParseResult {
    std::vector<std::string> fields;
    // 行级语法错误的原因；非空时 fields 不可用。
    std::string error;
    // 行末引号没闭上。这种行可能是跨行字段的开头，后面的行没法再按记录切分，
    // 调用方要把它当文件级失败处理。
    bool unclosed_quote{false};
};

// 按逐字符状态机解析一行 CSV，规则对齐常见的 CSV 引号语义：
// 引号字段里的逗号是普通字符，"" 表示一个字面双引号，反斜杠没有特殊含义；
// 没加引号的字段里出现双引号、闭引号后面还跟着内容，都算语法错误。
// 行结束符已由调用方去掉，这里只处理单行内容。
CsvLineParseResult parse_csv_line(std::string_view line) {
    enum class FieldState {
        Start,        // 字段还没开始，可能是个空字段
        Unquoted,     // 普通字段内容
        Quoted,       // 引号字段内容
        QuoteClosed,  // 闭引号已出现，后面只允许跟逗号或行尾
    };

    CsvLineParseResult result;
    std::string field;
    auto state = FieldState::Start;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        switch (state) {
        case FieldState::Start:
            if (ch == ',') {
                result.fields.push_back({});
            } else if (ch == '"') {
                state = FieldState::Quoted;
            } else {
                field.push_back(ch);
                state = FieldState::Unquoted;
            }
            break;
        case FieldState::Unquoted:
            if (ch == ',') {
                result.fields.push_back(field);
                field.clear();
                state = FieldState::Start;
            } else if (ch == '"') {
                result.error = "quote inside an unquoted field";
                return result;
            } else {
                field.push_back(ch);
            }
            break;
        case FieldState::Quoted:
            if (ch == '"') {
                if (index + 1 < line.size() && line[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    state = FieldState::QuoteClosed;
                }
            } else {
                field.push_back(ch);
            }
            break;
        case FieldState::QuoteClosed:
            if (ch == ',') {
                result.fields.push_back(field);
                field.clear();
                state = FieldState::Start;
            } else {
                result.error = "content after a closing quote";
                return result;
            }
            break;
        }
    }

    if (state == FieldState::Quoted) {
        result.unclosed_quote = true;
        return result;
    }
    // 行尾的最后一个字段（包括空字段）在这里补进去，尾部空字段才不会丢。
    result.fields.push_back(std::move(field));
    return result;
}

// 表头前三列固定，后面至少还要一列；列名不能为空，也不能重复，
// 不然 payload 的键会撞在一起。
bool is_valid_header(const std::vector<std::string>& fields) {
    if (fields.size() < 4 || fields[0] != "station_code" ||
        fields[1] != "device_code" || fields[2] != "record_time") {
        return false;
    }
    std::unordered_set<std::string> seen;
    for (const auto& name : fields) {
        if (name.empty() || !seen.insert(name).second) {
            return false;
        }
    }
    return true;
}

// getline 只去 LF，CRLF 行会残留行尾的 \r，先摘掉再交给状态机。
void strip_line_ending(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

// BOM 只出现在文件开头，由第一行（表头）处理。
void strip_bom(std::string& line) {
    if (line.size() >= kUtf8Bom.size() &&
        line.compare(0, kUtf8Bom.size(), kUtf8Bom) == 0) {
        line.erase(0, kUtf8Bom.size());
    }
}

}  // namespace

ParseResult CsvObservationParser::parse(const RawFileContext& context) {
    ParseResult result;
    std::ifstream input(context.local_path);
    if (!input.is_open()) {
        result.status = labbridge::core::Status::failure("failed to open csv file");
        return result;
    }

    std::string line;
    if (!std::getline(input, line)) {
        result.status = labbridge::core::Status::failure("csv file is empty");
        return result;
    }
    strip_line_ending(line);
    strip_bom(line);

    const auto header = parse_csv_line(line);
    if (header.unclosed_quote || !header.error.empty() ||
        !is_valid_header(header.fields)) {
        result.status = labbridge::core::Status::failure("csv header is invalid");
        return result;
    }

    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        strip_line_ending(line);
        if (line.empty()) {
            continue;
        }

        const auto parsed = parse_csv_line(line);
        if (parsed.unclosed_quote) {
            result.status = labbridge::core::Status::failure(
                "csv line " + std::to_string(line_number) +
                " has an unclosed quote; multi-line fields are not supported");
            return result;
        }
        if (!parsed.error.empty()) {
            result.errors.push_back(
                "line " + std::to_string(line_number) + " " + parsed.error);
            continue;
        }
        // 列数和表头对不上就跳过整行：截断会丢数据，补齐会造数据。
        if (parsed.fields.size() != header.fields.size()) {
            result.errors.push_back(
                "line " + std::to_string(line_number) + " has " +
                std::to_string(parsed.fields.size()) +
                " fields but the header has " +
                std::to_string(header.fields.size()));
            continue;
        }

        labbridge::core::ParsedRecord record;
        record.station_code = parsed.fields[0];
        record.device_code = parsed.fields[1];
        record.record_time = parsed.fields[2];
        record.payload_json = make_payload_json(header.fields, parsed.fields);
        result.records.push_back(std::move(record));
    }
    if (input.bad()) {
        result.status = labbridge::core::Status::failure(
            "failed while reading csv file");
        return result;
    }
    result.status = labbridge::core::Status::success();
    return result;
}

}  // namespace labbridge::agent
