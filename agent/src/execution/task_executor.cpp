#include "labbridge/agent/execution/task_executor.h"

#include "labbridge/agent/collectors/local_dir_collector.h"
#include "labbridge/core/logging.h"
#include "labbridge/agent/parsers/csv_parser.h"
#include "labbridge/agent/qc/basic_qc_rules.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace labbridge::agent {
namespace {
constexpr std::string_view kComponent = "task-executor";

constexpr std::size_t kMaximumErrorDetails = 5;
constexpr std::size_t kMaximumErrorSummaryBytes = 512;

struct SourceSpec {
    labbridge::core::fs::path root_path;
    std::string extension;
};

struct ArchivedWork {
    ArchivedLocalFile file;
    std::string raw_file_id;
    bool parsed_without_errors{false};
};

class ErrorSummary final {
public:
    void add(std::string detail) {
        ++total_;
        if (details_.size() < kMaximumErrorDetails) {
            details_.push_back(std::move(detail));
        }
    }

    bool empty() const noexcept {
        return total_ == 0;
    }

    std::string text() const {
        if (empty()) {
            return {};
        }
        std::ostringstream output;
        output << total_ << " error(s)";
        for (const auto& detail : details_) {
            output << "; " << detail;
        }
        if (total_ > details_.size()) {
            output << "; " << (total_ - details_.size())
                   << " additional error(s) omitted";
        }
        auto result = output.str();
        if (result.size() > kMaximumErrorSummaryBytes) {
            result.resize(kMaximumErrorSummaryBytes - 3);
            result += "...";
        }
        return result;
    }

private:
    std::size_t total_{0};
    std::vector<std::string> details_;
};

std::string format_utc(std::chrono::system_clock::time_point timestamp) {
    const auto raw_time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw_time);
#else
    gmtime_r(&raw_time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

bool path_is_within(const labbridge::core::fs::path& path,
                    const labbridge::core::fs::path& root) {
    auto path_part = path.begin();
    for (auto root_part = root.begin(); root_part != root.end();
         ++root_part, ++path_part) {
        if (path_part == path.end() || *path_part != *root_part) {
            return false;
        }
    }
    return true;
}

SourceSpec parse_source_spec(
    const labbridge::core::TaskConfig& task,
    const std::vector<labbridge::core::fs::path>& allowed_roots) {
    const auto config =
        nlohmann::json::parse(task.data_source.config_json);
    if (!config.is_object() || !config.contains("root_path") ||
        !config.at("root_path").is_string() ||
        !config.contains("extension") ||
        !config.at("extension").is_string()) {
        throw std::invalid_argument(
            "data source config requires string root_path and extension");
    }

    SourceSpec spec;
    spec.root_path = config.at("root_path").get<std::string>();
    spec.extension = config.at("extension").get<std::string>();
    if (!spec.root_path.is_absolute()) {
        throw std::invalid_argument("data source root_path must be absolute");
    }
    spec.root_path = labbridge::core::fs::weakly_canonical(spec.root_path);
    const auto allowed = std::any_of(
        allowed_roots.begin(), allowed_roots.end(),
        [&spec](const auto& root) {
            return path_is_within(spec.root_path, root);
        });
    if (!allowed) {
        throw std::invalid_argument(
            "data source root_path is outside allowed_local_roots");
    }
    if (spec.extension != ".csv" ||
        spec.extension.find('/') != std::string::npos ||
        spec.extension.find('\\') != std::string::npos) {
        throw std::invalid_argument(
            "data source extension must be .csv");
    }
    return spec;
}

void validate_execution_types(const labbridge::core::TaskConfig& task) {
    if (task.task_type != "local_file_import" ||
        task.parser_type != "csv_observation" ||
        task.data_source.type != labbridge::core::SourceType::LocalDirectory) {
        throw std::invalid_argument(
            "task requires local_directory, local_file_import and csv_observation");
    }
    for (const auto& rule : task.qc_rules) {
        if (rule.rule_type != "required_fields" &&
            rule.rule_type != "basic_timestamp_format") {
            throw std::invalid_argument(
                "unsupported QC rule type: " + rule.rule_type);
        }
    }
}

TaskRunReportQcResult run_rule(
    const labbridge::core::QcRuleConfig& config,
    const labbridge::core::ParsedRecord& record) {
    QcCheckResult checked;
    if (config.rule_type == "required_fields") {
        RequiredFieldsRule rule;
        checked = rule.check(record);
    } else {
        BasicTimestampRule rule;
        checked = rule.check(record);
    }

    const bool passed = checked.level == QcLevel::Pass;
    return {
        config.id,
        passed ? "pass" : "failed",
        passed ? "passed" : "failed",
        checked.message,
    };
}

}  // namespace

TaskExecutor::TaskExecutor(
    ITaskExecutionClient& client,
    labbridge::core::fs::path work_dir,
    std::vector<labbridge::core::fs::path> allowed_local_roots,
    std::size_t fingerprint_capacity)
    : TaskExecutor(
          client,
          std::move(work_dir),
          std::move(allowed_local_roots),
          [] { return std::chrono::system_clock::now(); },
          fingerprint_capacity) {}

TaskExecutor::TaskExecutor(
    ITaskExecutionClient& client,
    labbridge::core::fs::path work_dir,
    std::vector<labbridge::core::fs::path> allowed_local_roots,
    NowFunction now,
    std::size_t fingerprint_capacity)
    : client_(client),
      archive_store_(std::move(work_dir)),
      now_(std::move(now)),
      fingerprint_capacity_(fingerprint_capacity) {
    if (!now_ || fingerprint_capacity_ == 0 ||
        allowed_local_roots.empty()) {
        throw std::invalid_argument(
            "executor requires a clock, allowed root and positive fingerprint capacity");
    }
    allowed_local_roots_.reserve(allowed_local_roots.size());
    for (auto& root : allowed_local_roots) {
        if (!root.is_absolute()) {
            throw std::invalid_argument("allowed local roots must be absolute");
        }
        allowed_local_roots_.push_back(
            labbridge::core::fs::weakly_canonical(std::move(root)));
    }
}

TaskExecutor::TaskExecutor(
    ITaskExecutionClient& client,
    IReliableExecutionStore& queue_store,
    labbridge::core::fs::path work_dir,
    std::vector<labbridge::core::fs::path> allowed_local_roots,
    NowFunction now)
    : TaskExecutor(client,
                   std::move(work_dir),
                   std::move(allowed_local_roots),
                   std::move(now),
                   1) {
    queue_store_ = &queue_store;
}

RecoveredJob TaskExecutor::load_job(
    const std::string& execution_key) const {
    for (auto& job : queue_store_->recover_jobs()) {
        if (job.execution_key == execution_key) {
            return job;
        }
    }
    throw std::runtime_error("pending job disappeared");
}

void TaskExecutor::recover_pending_jobs() {
    if (queue_store_ == nullptr) {
        throw std::logic_error("recovery requires a reliable queue store");
    }
    const auto jobs = queue_store_->recover_jobs();
    for (const auto& job : jobs) {
        run_reliable_job(load_job(job.execution_key));
    }
}

void TaskExecutor::run_reliable_job(RecoveredJob job) {
    if (job.stage == "start_pending") {
        const auto started = client_.start_task_run(job.start_request);
        queue_store_->accept_start(job.execution_key, started.task_run_id);
        job = load_job(job.execution_key);
    }

    if (job.stage == "collecting" && job.files.empty()) {
        validate_execution_types(job.task);
        const auto source = parse_source_spec(job.task, allowed_local_roots_);
        LocalDirCollector collector{source.root_path, source.extension};
        const auto collected = collector.collect({
            job.task.id, job.task.node_code, job.task.data_source.config_json});
        if (!collected.status.ok) {
            throw std::runtime_error("collect failed: " + collected.status.message);
        }
        std::vector<PendingFilePlan> plan;
        int ordinal = 0;
        for (const auto& item : collected.items) {
            auto metadata = archive_store_.inspect(item);
            const auto fingerprint = job.task.id + "\n" + metadata.fingerprint;
            if (queue_store_->is_file_processed(job.task.id, fingerprint)) {
                continue;
            }
            const auto archive_path = archive_store_.plan_archive_path(
                job.task.id, job.task_run_id,
                static_cast<std::size_t>(ordinal + 1), metadata.original_name);
            plan.push_back({
                ordinal++,
                metadata.source_path.string(),
                metadata.original_name,
                metadata.source_mtime,
                metadata.size_bytes,
                metadata.file_hash,
                fingerprint,
                archive_path.string(),
            });
        }
        queue_store_->save_file_plan(job.execution_key, plan);
        job = load_job(job.execution_key);
    }

    if (job.stage == "collecting") {
        RawFileManifestRequest manifest;
        manifest.task_run_id = job.task_run_id;
        manifest.node_code = job.task.node_code;
        manifest.idempotency_key =
            make_manifest_idempotency_key(manifest.node_code, manifest.task_run_id);
        for (const auto& file : job.files) {
            LocalFileMetadata metadata{
                file.source_path,
                file.original_name,
                file.file_hash,
                file.size_bytes,
                file.source_mtime,
                file.fingerprint.substr(job.task.id.size() + 1),
            };
            archive_store_.recover_archive(metadata, file.archive_path);
            queue_store_->mark_file_archived(job.execution_key, file.ordinal);
            manifest.files.push_back({
                file.original_name, file.file_hash, file.archive_path,
                file.size_bytes, file.source_mtime, "archived_local"});
        }
        if (!manifest.files.empty()) {
            queue_store_->save_manifest(job.execution_key, manifest);
            job = load_job(job.execution_key);
        }
    }

    if (job.stage == "manifest_pending") {
        const auto result =
            client_.report_raw_file_manifest(job.manifest_request);
        queue_store_->accept_manifest(job.execution_key, result.raw_file_ids);
        job = load_job(job.execution_key);
    }

    if (job.stage == "collecting" || job.stage == "report_building") {
        TaskRunReportRequest report;
        report.task_run_id = job.task_run_id;
        report.node_code = job.task.node_code;
        report.idempotency_key =
            make_report_idempotency_key(report.node_code, report.task_run_id);
        ErrorSummary errors;
        std::vector<bool> parsed_without_errors;
        CsvObservationParser parser;
        for (const auto& file : job.files) {
            const auto parsed = parser.parse({
                report.task_run_id, file.raw_file_id, file.archive_path});
            const bool clean = parsed.status.ok && parsed.errors.empty();
            parsed_without_errors.push_back(clean);
            if (!parsed.status.ok) {
                ++report.items_total;
                ++report.items_failed;
                errors.add(file.original_name + ": " + parsed.status.message);
                continue;
            }
            report.items_total += static_cast<int>(
                parsed.records.size() + parsed.errors.size());
            report.items_success += static_cast<int>(parsed.records.size());
            report.items_failed += static_cast<int>(parsed.errors.size());
            for (const auto& error : parsed.errors) {
                errors.add(file.original_name + ": " + error);
            }
            for (const auto& record : parsed.records) {
                TaskRunReportParsedRecord result;
                result.raw_file_id = file.raw_file_id;
                result.record = record;
                for (const auto& rule : job.task.qc_rules) {
                    result.qc_results.push_back(run_rule(rule, record));
                }
                report.parsed_records.push_back(std::move(result));
            }
        }
        report.status = errors.empty()
            ? labbridge::core::TaskRunStatus::Succeeded
            : labbridge::core::TaskRunStatus::Failed;
        report.finished_at = format_utc(now_());
        report.error_summary = errors.text();
        queue_store_->save_report(
            job.execution_key, report, parsed_without_errors);
        job = load_job(job.execution_key);
    }

    if (job.stage == "report_pending") {
        client_.report_task_run(job.report_request);
        queue_store_->complete_job(job.execution_key);
    }
}
void TaskExecutor::execute(ScheduledTaskExecution execution) {
    if (stop_requested_.load(std::memory_order_acquire)) {
        return;
    }
    if (queue_store_ != nullptr) {
        if (queue_store_->has_capacity() == false) { return; }
        const auto scheduled_for = format_utc(execution.scheduled_for);
        StartTaskRunRequest request{
            execution.task.node_code,
            execution.task.id,
            make_scheduled_execution_key(
                execution.task.node_code, execution.task.id, scheduled_for),
            scheduled_for,
            format_utc(now_()),
            "scheduled",
        };
        queue_store_->begin_job(execution.task, request);
        run_reliable_job(load_job(request.execution_key));
        return;
    }

    const auto started_at = format_utc(now_());
    const auto scheduled_for = format_utc(execution.scheduled_for);
    const auto start = client_.start_task_run({
        execution.task.node_code,
        execution.task.id,
        make_scheduled_execution_key(
            execution.task.node_code, execution.task.id, scheduled_for),
        scheduled_for,
        started_at,
        "scheduled",
    });

    TaskRunReportRequest report;
    report.task_run_id = start.task_run_id;
    report.node_code = execution.task.node_code;
    report.idempotency_key =
        make_report_idempotency_key(report.node_code, report.task_run_id);

    ErrorSummary errors;
    std::vector<ArchivedWork> archived;

    try {
        if (stop_requested_.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "agent stopping after task run start; collection skipped");
        }
        validate_execution_types(execution.task);
        const auto source =
            parse_source_spec(execution.task, allowed_local_roots_);
        LocalDirCollector collector{source.root_path, source.extension};
        const auto collected = collector.collect({
            execution.task.id,
            execution.task.node_code,
            execution.task.data_source.config_json,
        });
        if (!collected.status.ok) {
            throw std::runtime_error(
                "collect failed: " + collected.status.message);
        }

        std::size_t ordinal = 0;
        for (const auto& item : collected.items) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                throw std::runtime_error(
                    "agent stopping; remaining files were not collected");
            }
            try {
                auto metadata = archive_store_.inspect(item);
                const auto fingerprint =
                    execution.task.id + "\n" + metadata.fingerprint;
                if (was_processed(execution.task.id, fingerprint)) {
                    continue;
                }
                archived.push_back({
                    archive_store_.archive(
                        execution.task.id, report.task_run_id,
                        ++ordinal, metadata),
                    {},
                    false,
                });
            } catch (const std::exception& error) {
                ++report.items_total;
                ++report.items_failed;
                errors.add(item.original_name + ": " + error.what());
            }
        }

        if (!archived.empty()) {
            RawFileManifestRequest manifest;
            manifest.task_run_id = report.task_run_id;
            manifest.node_code = report.node_code;
            manifest.idempotency_key =
                make_manifest_idempotency_key(
                    manifest.node_code, manifest.task_run_id);
            for (const auto& work : archived) {
                manifest.files.push_back({
                    work.file.source.original_name,
                    work.file.source.file_hash,
                    work.file.archive_path.string(),
                    work.file.source.size_bytes,
                    work.file.source.source_mtime,
                    "archived_local",
                });
            }
            const auto manifest_result =
                client_.report_raw_file_manifest(manifest);
            if (manifest_result.raw_file_ids.size() != archived.size()) {
                throw std::runtime_error(
                    "manifest raw_file_ids count does not match archived files");
            }
            for (std::size_t index = 0; index < archived.size(); ++index) {
                archived[index].raw_file_id =
                    manifest_result.raw_file_ids[index];
            }
        }

        CsvObservationParser parser;
        for (auto& work : archived) {
            const auto parsed = parser.parse({
                report.task_run_id,
                work.raw_file_id,
                work.file.archive_path.string(),
            });
            if (!parsed.status.ok) {
                ++report.items_total;
                ++report.items_failed;
                errors.add(
                    work.file.source.original_name + ": " +
                    parsed.status.message);
                continue;
            }

            report.items_total += static_cast<int>(
                parsed.records.size() + parsed.errors.size());
            report.items_success +=
                static_cast<int>(parsed.records.size());
            report.items_failed +=
                static_cast<int>(parsed.errors.size());
            for (const auto& error : parsed.errors) {
                errors.add(
                    work.file.source.original_name + ": " + error);
            }

            for (const auto& record : parsed.records) {
                TaskRunReportParsedRecord result;
                result.raw_file_id = work.raw_file_id;
                result.record = record;
                for (const auto& rule : execution.task.qc_rules) {
                    result.qc_results.push_back(run_rule(rule, record));
                }
                report.parsed_records.push_back(std::move(result));
            }
            work.parsed_without_errors = parsed.errors.empty();
        }
    } catch (const std::exception& error) {
        if (report.items_total == 0) {
            report.items_total = 1;
            report.items_failed = 1;
        }
        errors.add(error.what());
    }

    report.status = errors.empty()
                        ? labbridge::core::TaskRunStatus::Succeeded
                        : labbridge::core::TaskRunStatus::Failed;
    report.finished_at = format_utc(now_());
    report.error_summary = errors.text();
    if (stop_requested_.load(std::memory_order_acquire)) {
        labbridge::core::log_warn(
            kComponent,
            "draining=true; task_run_id=" + report.task_run_id +
                "; report_idempotency_key=" + report.idempotency_key);
    }

    client_.report_task_run(report);
    // 只有控制面明确接收终态报告后，成功文件才进入进程内去重集合。
    // 网络结果不确定时保留再次投递机会，不能伪造“已处理”。

    for (const auto& work : archived) {
        if (work.parsed_without_errors) {
            remember_processed(
                execution.task.id,
                execution.task.id + "\n" + work.file.source.fingerprint);
        }
    }
}
void TaskExecutor::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    client_.request_stop();
}

void TaskExecutor::forget_task(const std::string& task_id) {
    const std::lock_guard<std::mutex> lock{processed_fingerprints_mutex_};
    processed_fingerprints_.erase(task_id);
}

bool TaskExecutor::was_processed(
    const std::string& task_id,
    const std::string& fingerprint) const {
    const std::lock_guard<std::mutex> lock{processed_fingerprints_mutex_};
    const auto found = processed_fingerprints_.find(task_id);
    return found != processed_fingerprints_.end() &&
           std::find(
               found->second.begin(), found->second.end(), fingerprint) !=
               found->second.end();
}

void TaskExecutor::remember_processed(
    const std::string& task_id,
    std::string fingerprint) {
    const std::lock_guard<std::mutex> lock{processed_fingerprints_mutex_};
    auto& fingerprints = processed_fingerprints_[task_id];
    if (std::find(
            fingerprints.begin(), fingerprints.end(), fingerprint) !=
        fingerprints.end()) {
        return;
    }
    if (fingerprints.size() == fingerprint_capacity_) {
        std::clog << "task_id=" << task_id
                  << " successful fingerprint capacity reached; evicting oldest"
                  << std::endl;
        fingerprints.pop_front();
    }
    fingerprints.push_back(std::move(fingerprint));
}

}  // namespace labbridge::agent
