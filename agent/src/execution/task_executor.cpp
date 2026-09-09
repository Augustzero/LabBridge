#include "labbridge/agent/execution/task_executor.h"

#include "labbridge/agent/collectors/local_dir_collector.h"
#include "labbridge/core/logging.h"
#include "labbridge/core/utc_time.h"
#include "labbridge/agent/parsers/csv_parser.h"
#include "labbridge/agent/qc/basic_qc_rules.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
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
            return labbridge::core::is_within(spec.root_path, root);
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
    IReliableExecutionStore& queue_store,
    labbridge::core::fs::path work_dir,
    std::vector<labbridge::core::fs::path> allowed_local_roots,
    NowFunction now)
    : client_(client),
      queue_store_(queue_store),
      archive_store_(std::move(work_dir)),
      now_(std::move(now)) {
    if (!now_ || allowed_local_roots.empty()) {
        throw std::invalid_argument(
            "executor requires a clock and at least one allowed local root");
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

RecoveredJob TaskExecutor::load_job(
    const std::string& execution_key) const {
    return queue_store_.load_job(execution_key);
}

void TaskExecutor::recover_pending_jobs() {
    for (const auto& job : queue_store_.recover_jobs()) {
        run_reliable_job(job);
    }
}

RecoveredJob TaskExecutor::run_collecting_stage(
    const RecoveredJob& input) const {
    RecoveredJob job = input;
    if (job.files.empty()) {
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
            if (queue_store_.is_file_processed(job.task.id, fingerprint)) {
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
        queue_store_.save_file_plan(job.execution_key, plan);
        job = load_job(job.execution_key);
    }

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
        queue_store_.mark_file_archived(job.execution_key, file.ordinal);
        manifest.files.push_back({
            file.original_name, file.file_hash, file.archive_path,
            file.size_bytes, file.source_mtime, "archived_local"});
    }
    if (!manifest.files.empty()) {
        queue_store_.save_manifest(job.execution_key, manifest);
        job = load_job(job.execution_key);
    }
    return job;
}

RecoveredJob TaskExecutor::save_collection_failure_report(
    const RecoveredJob& job, const std::string& error) const {
    // 失败可能发生在文件计划已部分提交之后，以队列中的最新作业状态为准。
    const auto current = load_job(job.execution_key);
    TaskRunReportRequest report;
    report.task_run_id = current.task_run_id;
    report.node_code = current.task.node_code;
    report.idempotency_key =
        make_report_idempotency_key(report.node_code, report.task_run_id);
    report.status = labbridge::core::TaskRunStatus::Failed;
    report.finished_at = labbridge::core::format_utc_timestamp(now_());
    report.items_total = 1;
    report.items_failed = 1;
    report.error_summary = error;
    // 采集阶段失败的作业没有任何成功解析的文件。
    const std::vector<bool> parsed_without_errors(current.files.size(), false);
    queue_store_.save_report(
        current.execution_key, report, parsed_without_errors);
    return load_job(current.execution_key);
}

void TaskExecutor::run_reliable_job(RecoveredJob job) {
    if (job.stage == "start_pending") {
        const auto started = client_.start_task_run(job.start_request);
        queue_store_.accept_start(job.execution_key, started.task_run_id);
        job = load_job(job.execution_key);
    }

    if (job.stage == "collecting") {
        try {
            job = run_collecting_stage(job);
        } catch (const ArchiveConflictError& error) {
            // 归档证据与持久化指纹不一致：自动重试可能覆盖现场证据，转人工处理。
            queue_store_.mark_requires_attention(
                job.execution_key, error.what());
            labbridge::core::log_warn(
                kComponent,
                "job requires attention; execution_key=" + job.execution_key +
                    "; reason=" + error.what());
            return;
        } catch (const AgentQueueError&) {
            // 队列库自身故障按 Phase 024 语义传播到进程边界。
            throw;
        } catch (const std::exception& error) {
            // 采集目录缺失、数据源配置非法、源文件消失等外部条件
            // 折叠为终态 failed report，Agent 继续运行。
            job = save_collection_failure_report(job, error.what());
        }
    }

    if (job.stage == "manifest_pending") {
        const auto result =
            client_.report_raw_file_manifest(job.manifest_request);
        queue_store_.accept_manifest(job.execution_key, result.raw_file_ids);
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
        report.finished_at = labbridge::core::format_utc_timestamp(now_());
        report.error_summary = errors.text();
        queue_store_.save_report(
            job.execution_key, report, parsed_without_errors);
        job = load_job(job.execution_key);
    }

    if (job.stage == "report_pending") {
        client_.report_task_run(job.report_request);
        queue_store_.complete_job(job.execution_key);
    }
}

void TaskExecutor::execute(ScheduledTaskExecution execution) {
    if (stop_requested_.load(std::memory_order_acquire)) {
        return;
    }
    if (queue_store_.has_capacity() == false) {
        // 队列容量已满时该调度槽被丢弃，必须留下可观测线索。
        labbridge::core::log_warn(
            kComponent,
            "pending job capacity reached; skipping scheduled slot; node_code=" +
                execution.task.node_code + "; task_id=" + execution.task.id +
                "; scheduled_for=" +
                labbridge::core::format_utc_timestamp(execution.scheduled_for));
        return;
    }
    const auto scheduled_for =
        labbridge::core::format_utc_timestamp(execution.scheduled_for);
    StartTaskRunRequest request{
        execution.task.node_code,
        execution.task.id,
        make_scheduled_execution_key(
            execution.task.node_code, execution.task.id, scheduled_for),
        scheduled_for,
        labbridge::core::format_utc_timestamp(now_()),
        "scheduled",
    };
    queue_store_.begin_job(execution.task, request);
    run_reliable_job(load_job(request.execution_key));
}

void TaskExecutor::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    client_.request_stop();
}

}  // namespace labbridge::agent
