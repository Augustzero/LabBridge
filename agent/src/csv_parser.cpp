#include "labbridge/agent/csv_parser.h"

#include <fstream>
#include <sstream>
#include <string>

namespace labbridge::agent {
namespace {

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream stream(line);
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

std::string make_payload_json(const std::vector<std::string>& header, const std::vector<std::string>& row) {
    std::ostringstream payload;
    payload << "{";
    bool first = true;
    for (std::size_t index = 3; index < header.size() && index < row.size(); ++index) {
        if (!first) {
            payload << ",";
        }
        payload << "\"" << header[index] << "\":\"" << row[index] << "\"";
        first = false;
    }
    payload << "}";
    return payload.str();
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

    const auto header = split_csv_line(line);
    if (header.size() < 4) {
        result.status = labbridge::core::Status::failure("csv header is invalid");
        return result;
    }

    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }

        const auto row = split_csv_line(line);
        if (row.size() < 3) {
            result.errors.push_back("line " + std::to_string(line_number) + " has fewer than 3 fields");
            continue;
        }

        labbridge::core::ParsedRecord record;
        record.station_code = row[0];
        record.device_code = row[1];
        record.record_time = row[2];
        record.payload_json = make_payload_json(header, row);
        result.records.push_back(std::move(record));
    }

    result.status = labbridge::core::Status::success();
    return result;
}

}  // namespace labbridge::agent

