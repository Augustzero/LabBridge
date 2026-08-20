#include "labbridge/server/application/management_query_service.h"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace labbridge::server {
namespace {

using labbridge::core::Status;
using labbridge::core::StatusCode;

bool is_positive_id(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    unsigned long long parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} &&
           result.ptr == value.data() + value.size() &&
           parsed > 0 &&
           parsed <= static_cast<unsigned long long>(
               std::numeric_limits<long long>::max());
}

Status validate_page(const PageInput& page) {
    if (page.limit < 1 || page.limit > 100) {
        return Status::failure("limit must be between 1 and 100");
    }
    if (page.cursor.has_value() && !is_positive_id(*page.cursor)) {
        return Status::failure("cursor must be a positive integer");
    }
    return Status::success();
}

ManagementPageRequest repository_page(const PageInput& page) {
    return {page.limit + 1, page.cursor};
}

template <typename T, typename IdReader>
ManagementPage<T> trim_page(
    std::vector<T> rows,
    int limit,
    IdReader read_id) {
    ManagementPage<T> page;
    page.has_more = rows.size() > static_cast<std::size_t>(limit);
    if (page.has_more) {
        rows.resize(static_cast<std::size_t>(limit));
    }
    page.items = std::move(rows);
    if (page.has_more && !page.items.empty()) {
        page.next_cursor = read_id(page.items.back());
    }
    return page;
}

// Howard Hinnant 的 civil-date 换算公式，避免依赖进程本地时区。
long long days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era =
        static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
        day_of_year;
    return static_cast<long long>(era) * 146097 +
           static_cast<long long>(day_of_era) - 719468;
}

std::optional<std::chrono::system_clock::time_point> parse_utc_timestamp(
    const std::string& value) {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') {
        return std::nullopt;
    }

    auto number = [&value](std::size_t offset, std::size_t count)
        -> std::optional<int> {
        int parsed = 0;
        const auto begin = value.data() + offset;
        const auto result = std::from_chars(begin, begin + count, parsed);
        if (result.ec != std::errc{} || result.ptr != begin + count) {
            return std::nullopt;
        }
        return parsed;
    };

    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second ||
        *month < 1 || *month > 12 || *day < 1 || *day > 31 ||
        *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }

    const long long seconds =
        days_from_civil(*year, static_cast<unsigned>(*month),
                        static_cast<unsigned>(*day)) * 86400 +
        *hour * 3600 + *minute * 60 + *second;
    return std::chrono::system_clock::time_point{
        std::chrono::seconds{seconds}};
}

std::string format_utc_timestamp(
    std::chrono::system_clock::time_point time) {
    const auto raw = std::chrono::system_clock::to_time_t(time);
    std::tm utc{};
    gmtime_r(&raw, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

labbridge::core::NodeStatus effective_status(
    const NodeRecord& node,
    std::chrono::system_clock::time_point now,
    int offline_after_seconds) {
    if (node.status != labbridge::core::NodeStatus::Online) {
        return labbridge::core::NodeStatus::Offline;
    }
    const auto heartbeat = parse_utc_timestamp(node.last_heartbeat_at);
    if (!heartbeat.has_value() || *heartbeat > now) {
        return labbridge::core::NodeStatus::Offline;
    }
    return now - *heartbeat > std::chrono::seconds{offline_after_seconds}
        ? labbridge::core::NodeStatus::Offline
        : labbridge::core::NodeStatus::Online;
}

bool is_stale(
    const TaskRunRecord& run,
    std::chrono::system_clock::time_point now,
    int stale_after_seconds) {
    if (run.status != labbridge::core::TaskRunStatus::Running) {
        return false;
    }
    const auto started_at = parse_utc_timestamp(run.started_at);
    return started_at.has_value() && *started_at <= now &&
           now - *started_at > std::chrono::seconds{stale_after_seconds};
}

std::optional<labbridge::core::NodeStatus> parse_node_status(
    const std::optional<std::string>& value) {
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value == "online") {
        return labbridge::core::NodeStatus::Online;
    }
    if (*value == "offline") {
        return labbridge::core::NodeStatus::Offline;
    }
    return std::nullopt;
}

std::optional<labbridge::core::TaskRunStatus> parse_run_status(
    const std::optional<std::string>& value) {
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value == "pending") {
        return labbridge::core::TaskRunStatus::Pending;
    }
    if (*value == "running") {
        return labbridge::core::TaskRunStatus::Running;
    }
    if (*value == "succeeded") {
        return labbridge::core::TaskRunStatus::Succeeded;
    }
    if (*value == "failed") {
        return labbridge::core::TaskRunStatus::Failed;
    }
    return std::nullopt;
}

Status validate_node_code(const std::string& node_code) {
    if (node_code.empty() || node_code.size() > 64) {
        return Status::failure("node_code must contain 1 to 64 characters");
    }
    return Status::success();
}

Status not_found(std::string message) {
    return Status::failure(StatusCode::NotFound, std::move(message));
}

}  // namespace

ManagementQueryService::ManagementQueryService(
    IManagementQueryRepository& repository,
    std::chrono::system_clock::time_point now,
    int node_offline_after_seconds,
    int task_run_stale_after_seconds)
    : repository_(repository),
      now_(now),
      node_offline_after_seconds_(node_offline_after_seconds),
      task_run_stale_after_seconds_(task_run_stale_after_seconds) {}

ManagementPageResult<ManagementNode> ManagementQueryService::list_nodes(
    const NodeListRequest& request) const {
    const auto page_status = validate_page(request.page);
    if (!page_status.ok) {
        return {page_status, {}};
    }
    const auto status = parse_node_status(request.status);
    if (request.status.has_value() && !status.has_value()) {
        return {Status::failure("status must be online or offline"), {}};
    }

    NodeListFilter filter{
        status,
        format_utc_timestamp(now_),
        node_offline_after_seconds_,
    };
    auto records = repository_.list_nodes(
        filter, repository_page(request.page));
    std::vector<ManagementNode> nodes;
    nodes.reserve(records.size());
    for (auto& record : records) {
        const auto derived = effective_status(
            record, now_, node_offline_after_seconds_);
        nodes.push_back({std::move(record), derived});
    }
    return {
        Status::success(),
        trim_page(
            std::move(nodes),
            request.page.limit,
            [](const ManagementNode& node) { return node.record.id; }),
    };
}

ManagementItemResult<ManagementNodeSummary>
ManagementQueryService::find_node(const std::string& node_code) const {
    const auto status = validate_node_code(node_code);
    if (!status.ok) {
        return {status, std::nullopt};
    }
    auto summary = repository_.find_node_summary(node_code);
    if (!summary.has_value()) {
        return {not_found("node is not found"), std::nullopt};
    }
    const auto derived = effective_status(
        summary->node, now_, node_offline_after_seconds_);
    return {
        Status::success(),
        ManagementNodeSummary{std::move(*summary), derived},
    };
}

ManagementPageResult<DataSourceRecord>
ManagementQueryService::list_data_sources(
    const NodeScopedListRequest& request) const {
    const auto node_status = validate_node_code(request.node_code);
    const auto page_status = validate_page(request.page);
    if (!node_status.ok) {
        return {node_status, {}};
    }
    if (!page_status.ok) {
        return {page_status, {}};
    }
    if (!repository_.find_node_summary(request.node_code).has_value()) {
        return {not_found("node is not found"), {}};
    }
    auto records = repository_.list_data_sources_by_node(
        request.node_code,
        {request.enabled},
        repository_page(request.page));
    return {
        Status::success(),
        trim_page(
            std::move(records),
            request.page.limit,
            [](const DataSourceRecord& record) { return record.id; }),
    };
}

ManagementPageResult<QcRuleRecord>
ManagementQueryService::list_qc_rules(
    const QcRuleListRequest& request) const {
    const auto status = validate_page(request.page);
    if (!status.ok) {
        return {status, {}};
    }
    auto records = repository_.list_qc_rules(
        {request.enabled}, repository_page(request.page));
    return {
        Status::success(),
        trim_page(
            std::move(records),
            request.page.limit,
            [](const QcRuleRecord& record) { return record.id; }),
    };
}

ManagementPageResult<TaskRecord> ManagementQueryService::list_tasks(
    const NodeScopedListRequest& request) const {
    const auto node_status = validate_node_code(request.node_code);
    const auto page_status = validate_page(request.page);
    if (!node_status.ok) {
        return {node_status, {}};
    }
    if (!page_status.ok) {
        return {page_status, {}};
    }
    if (!repository_.find_node_summary(request.node_code).has_value()) {
        return {not_found("node is not found"), {}};
    }
    auto records = repository_.list_tasks_by_node(
        request.node_code,
        {request.enabled},
        repository_page(request.page));
    return {
        Status::success(),
        trim_page(
            std::move(records),
            request.page.limit,
            [](const TaskRecord& record) { return record.id; }),
    };
}

ManagementPageResult<ManagementTaskRun>
ManagementQueryService::list_task_runs(
    const TaskRunListRequest& request) const {
    const auto node_status = validate_node_code(request.node_code);
    const auto page_status = validate_page(request.page);
    if (!node_status.ok) {
        return {node_status, {}};
    }
    if (!page_status.ok) {
        return {page_status, {}};
    }
    const auto run_status = parse_run_status(request.status);
    if (request.status.has_value() && !run_status.has_value()) {
        return {Status::failure(
            "status must be pending, running, succeeded or failed"), {}};
    }
    if (request.task_id.has_value() &&
        !is_positive_id(*request.task_id)) {
        return {Status::failure("task_id must be a positive integer"), {}};
    }
    if (!repository_.find_node_summary(request.node_code).has_value()) {
        return {not_found("node is not found"), {}};
    }
    if (request.task_id.has_value()) {
        const auto task = repository_.find_task(*request.task_id);
        if (!task.has_value() || task->node_code != request.node_code) {
            return {not_found("task is not found"), {}};
        }
    }

    auto records = repository_.list_task_runs_by_node(
        {request.node_code, request.task_id, run_status},
        repository_page(request.page));
    std::vector<ManagementTaskRun> runs;
    runs.reserve(records.size());
    for (auto& record : records) {
        const bool stale = is_stale(
            record, now_, task_run_stale_after_seconds_);
        runs.push_back({
            std::move(record),
            stale,
            task_run_stale_after_seconds_,
        });
    }
    return {
        Status::success(),
        trim_page(
            std::move(runs),
            request.page.limit,
            [](const ManagementTaskRun& run) { return run.record.id; }),
    };
}

ManagementItemResult<ManagementTaskRunSummary>
ManagementQueryService::find_task_run(
    const std::string& node_code,
    const std::string& task_run_id) const {
    const auto node_status = validate_node_code(node_code);
    if (!node_status.ok) {
        return {node_status, std::nullopt};
    }
    if (!is_positive_id(task_run_id)) {
        return {
            Status::failure("task_run_id must be a positive integer"),
            std::nullopt,
        };
    }
    auto summary = repository_.find_task_run_summary(task_run_id);
    if (!summary.has_value() ||
        summary->task_run.node_code != node_code) {
        return {not_found("task run is not found"), std::nullopt};
    }
    const bool stale = is_stale(
        summary->task_run, now_, task_run_stale_after_seconds_);
    return {
        Status::success(),
        ManagementTaskRunSummary{
            std::move(*summary),
            stale,
            task_run_stale_after_seconds_,
        },
    };
}

ManagementPageResult<RawFileRecord>
ManagementQueryService::list_raw_files(
    const RunScopedListRequest& request) const {
    const auto page_status = validate_page(request.page);
    if (!page_status.ok) {
        return {page_status, {}};
    }
    if (!is_positive_id(request.task_run_id)) {
        return {Status::failure(
            "task_run_id must be a positive integer"), {}};
    }
    if (!repository_.find_task_run_summary(
            request.task_run_id).has_value()) {
        return {not_found("task run is not found"), {}};
    }
    auto records = repository_.list_raw_files_by_run(
        request.task_run_id, repository_page(request.page));
    return {
        Status::success(),
        trim_page(
            std::move(records),
            request.page.limit,
            [](const RawFileRecord& record) { return record.id; }),
    };
}

ManagementPageResult<ParsedRecordRecord>
ManagementQueryService::list_parsed_records(
    const RunScopedListRequest& request) const {
    const auto page_status = validate_page(request.page);
    if (!page_status.ok) {
        return {page_status, {}};
    }
    if (!is_positive_id(request.task_run_id)) {
        return {Status::failure(
            "task_run_id must be a positive integer"), {}};
    }
    if (!repository_.find_task_run_summary(
            request.task_run_id).has_value()) {
        return {not_found("task run is not found"), {}};
    }
    auto records = repository_.list_parsed_records_by_run(
        request.task_run_id, repository_page(request.page));
    return {
        Status::success(),
        trim_page(
            std::move(records),
            request.page.limit,
            [](const ParsedRecordRecord& record) { return record.id; }),
    };
}

ManagementPageResult<QcResultRecord>
ManagementQueryService::list_qc_results(
    const QcResultListRequest& request) const {
    const auto page_status = validate_page(request.page);
    if (!page_status.ok) {
        return {page_status, {}};
    }
    if (!is_positive_id(request.task_run_id)) {
        return {Status::failure(
            "task_run_id must be a positive integer"), {}};
    }
    if (request.result.has_value() &&
        *request.result != "passed" &&
        *request.result != "failed") {
        return {Status::failure(
            "result must be passed or failed"), {}};
    }
    if (!repository_.find_task_run_summary(
            request.task_run_id).has_value()) {
        return {not_found("task run is not found"), {}};
    }
    auto records = repository_.list_qc_results_by_run(
        {request.task_run_id, request.result},
        repository_page(request.page));
    return {
        Status::success(),
        trim_page(
            std::move(records),
            request.page.limit,
            [](const QcResultRecord& record) { return record.id; }),
    };
}

ManagementPageResult<AlertRecord>
ManagementQueryService::list_alerts(
    const AlertListRequest& request) const {
    const auto node_status = validate_node_code(request.node_code);
    const auto page_status = validate_page(request.page);
    if (!node_status.ok) {
        return {node_status, {}};
    }
    if (!page_status.ok) {
        return {page_status, {}};
    }
    if (request.task_run_id.has_value() &&
        !is_positive_id(*request.task_run_id)) {
        return {Status::failure(
            "task_run_id must be a positive integer"), {}};
    }
    if (request.status.has_value() &&
        *request.status != "open" &&
        *request.status != "acknowledged" &&
        *request.status != "closed") {
        return {Status::failure(
            "status must be open, acknowledged or closed"), {}};
    }
    if (request.severity.has_value() &&
        *request.severity != "warning" &&
        *request.severity != "failed") {
        return {Status::failure(
            "severity must be warning or failed"), {}};
    }
    if (!repository_.find_node_summary(request.node_code).has_value()) {
        return {not_found("node is not found"), {}};
    }
    if (request.task_run_id.has_value()) {
        const auto run = repository_.find_task_run_summary(
            *request.task_run_id);
        if (!run.has_value() ||
            run->task_run.node_code != request.node_code) {
            return {not_found("task run is not found"), {}};
        }
    }
    auto records = repository_.list_alerts_by_node(
        {
            request.node_code,
            request.task_run_id,
            request.status,
            request.severity,
        },
        repository_page(request.page));
    return {
        Status::success(),
        trim_page(
            std::move(records),
            request.page.limit,
            [](const AlertRecord& record) { return record.id; }),
    };
}

}  // namespace labbridge::server
