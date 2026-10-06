#include "labbridge/agent/execution/task_executor.h"

#include "labbridge/agent/collectors/local_dir_collector.h"
#include "labbridge/core/logging.h"
#include "labbridge/core/utc_time.h"
#include "labbridge/agent/execution/reliable_delivery_client.h"
#include "labbridge/agent/execution/sha256.h"
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

// 计划行的实际读取路径：旧记录没有 input_path，按 source_path 读。
labbridge::core::fs::path input_path_of(const PendingFilePlan& file) {
    return file.input_path.empty()
        ? labbridge::core::fs::path{file.source_path}
        : labbridge::core::fs::path{file.input_path};
}

// 按源路径从 retry 快照取原归档引用；普通采集和 source 目标没有快照，
// 返回空串。
std::string inherited_archive_ref(
    const std::vector<RetryFileInput>& retry_files,
    const std::string& source_path) {
    for (const auto& target : retry_files) {
        if (target.source_path == source_path) {
            return target.archive_raw_file_id;
        }
    }
    return {};
}

// 归档重放目标必须落在自己的归档根目录里，且不是符号链接。
void validate_archive_input(const RetryFileInput& target,
                            const labbridge::core::fs::path& archive_root) {
    const labbridge::core::fs::path input{target.storage_path};
    if (labbridge::core::fs::is_symlink(
            labbridge::core::fs::symlink_status(input))) {
        throw std::runtime_error(
            "archived input is a symbolic link: " + target.storage_path);
    }
    const auto canonical =
        labbridge::core::fs::weakly_canonical(input);
    if (!labbridge::core::is_within(canonical, archive_root)) {
        throw std::runtime_error(
            "archived input is outside the archive root: " +
            target.storage_path);
    }
    if (!labbridge::core::fs::is_regular_file(canonical)) {
        throw std::runtime_error(
            "archived input is not a regular file: " + target.storage_path);
    }
    if (labbridge::core::fs::file_size(canonical) !=
            static_cast<std::uintmax_t>(target.size_bytes) ||
        sha256_file_hex(canonical) != target.file_hash) {
        throw std::runtime_error(
            "archived input does not match the recorded evidence: " +
            target.storage_path);
    }
}

// 定点补采目标：非符号链接，且仍在当前任务目录内（目录本身已过
// allowed_local_roots 校验）。路径丢失、被替换或越界都记该目标失败，
// 不搜索替代文件。
void validate_source_input(const RetryFileInput& target,
                           const labbridge::core::fs::path& root_path) {
    const labbridge::core::fs::path input{target.source_path};
    if (labbridge::core::fs::is_symlink(
            labbridge::core::fs::symlink_status(input))) {
        throw std::runtime_error(
            "retry target is a symbolic link: " + target.source_path);
    }
    const auto canonical =
        labbridge::core::fs::weakly_canonical(input);
    if (!labbridge::core::is_within(canonical, root_path)) {
        throw std::runtime_error(
            "retry target is outside the task directory: " +
            target.source_path);
    }
}

}  // namespace

TaskExecutor::TaskExecutor(
    ITaskExecutionClient& client,
    IReliableExecutionStore& queue_store,
    labbridge::core::fs::path work_dir,
    std::vector<labbridge::core::fs::path> allowed_local_roots,
    std::size_t max_files_per_run,
    NowFunction now)
    : client_(client),
      queue_store_(queue_store),
      archive_store_(std::move(work_dir)),
      max_files_per_run_(max_files_per_run),
      now_(std::move(now)) {
    if (!now_ || allowed_local_roots.empty() || max_files_per_run_ == 0) {
        throw std::invalid_argument(
            "executor requires a clock, at least one allowed local root "
            "and a positive file limit per run");
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
        // 每条恢复开始前先看停止标志，停止后不再启动新的积压作业。
        if (stop_requested_.load(std::memory_order_acquire)) {
            return;
        }
        try {
            run_reliable_job(job);
        } catch (const DeliveryAbandoned& error) {
            // 这条作业已被投递层标成 requires_attention，是它自己的终局；
            // 其他积压作业还得继续恢复，所以这里只记日志接着往下走。
            labbridge::core::log_warn(
                kComponent,
                "recovered job abandoned; execution_key=" + job.execution_key +
                    "; reason=" + error.what());
            if (stop_requested_.load(std::memory_order_acquire)) {
                return;
            }
        }
    }
}

void TaskExecutor::plan_from_directory(
    const RecoveredJob& job,
    std::vector<PendingFilePlan>& plan,
    std::vector<TaskRunReportFailedFile>& failures) const {
    validate_execution_types(job.task);
    const auto source = parse_source_spec(job.task, allowed_local_roots_);
    LocalDirCollector collector{source.root_path, source.extension};
    const auto collected = collector.collect({
        job.task.id, job.task.node_code, job.task.data_source.config_json});
    if (!collected.status.ok) {
        // 目录枚举 / 配置错误是运行级错误，不能猜测文件列表。
        throw std::runtime_error("collect failed: " + collected.status.message);
    }
    for (const auto& item : collected.items) {
        // 单轮最多带走 max_files_per_run 个文件，剩下的留给后续槽位；
        // 已选择但读取失败的文件也占名额，防止失败项绕过限制。
        if (plan.size() + failures.size() >= max_files_per_run_) {
            break;
        }
        LocalFileMetadata metadata;
        try {
            metadata = archive_store_.inspect(item);
        } catch (const std::exception& error) {
            failures.push_back({
                item.local_path, item.original_name, "read", error.what(), ""});
            continue;
        }
        const auto fingerprint = job.task.id + "\n" + metadata.fingerprint;
        // 已处理过或正被其他作业（含 requires_attention）占用的文件跳过，
        // 避免同槽位、跨重启重复入计划；未选中的不占名额。
        if (queue_store_.is_file_occupied(job.task.id, fingerprint)) {
            continue;
        }
        const auto archive_path = archive_store_.plan_archive_path(
            job.task.id, job.task_run_id,
            static_cast<std::size_t>(plan.size() + 1), metadata.original_name);
        plan.push_back({
            static_cast<int>(plan.size()),
            metadata.source_path.string(),
            metadata.original_name,
            metadata.source_mtime,
            metadata.size_bytes,
            metadata.file_hash,
            fingerprint,
            archive_path.string(),
        });
    }
}

void TaskExecutor::plan_from_retry_targets(
    const RecoveredJob& job,
    std::vector<PendingFilePlan>& plan,
    std::vector<TaskRunReportFailedFile>& failures) const {
    validate_execution_types(job.task);
    // source 目标按当前任务目录校验，配置仍要解析；archive 目标不用目录，
    // 但目录配置坏了属于任务不可执行，统一在开始就暴露。
    const auto source = parse_source_spec(job.task, allowed_local_roots_);
    const auto archive_root = archive_store_.work_dir() / "archive";

    for (const auto& target : job.retry_files) {
        PendingFilePlan entry;
        entry.ordinal = static_cast<int>(plan.size());
        entry.source_path = target.source_path;
        entry.original_name = target.original_name;
        entry.archive_path = archive_store_
                                 .plan_archive_path(
                                     job.task.id, job.task_run_id,
                                     static_cast<std::size_t>(plan.size() + 1),
                                     target.original_name)
                                 .string();

        if (target.input_type == "archive") {
            try {
                validate_archive_input(target, archive_root);
            } catch (const std::exception& error) {
                // 原归档缺失、损坏或越界：该目标失败，保留 archive 身份，
                // 不能降级成定点补采。
                failures.push_back({target.source_path, target.original_name,
                                    "archive", error.what(),
                                    target.archive_raw_file_id});
                continue;
            }
            entry.input_path = target.storage_path;
            entry.size_bytes = target.size_bytes;
            entry.file_hash = target.file_hash;
            entry.source_mtime = target.source_mtime;
            // 指纹用原输入身份（原来源路径 + 快照大小/时间/哈希），
            // 不能把归档路径或归档修改时间当成新采集的身份。
            entry.fingerprint =
                job.task.id + "\n" + target.source_path + "\n" +
                std::to_string(target.size_bytes) + "\n" +
                target.source_mtime + "\n" + target.file_hash;
        } else {
            try {
                validate_source_input(target, source.root_path);
                CollectedItem item;
                item.local_path = target.source_path;
                item.original_name = target.original_name;
                const auto metadata = archive_store_.inspect(item);
                entry.input_path = metadata.source_path.string();
                entry.size_bytes = metadata.size_bytes;
                entry.file_hash = metadata.file_hash;
                entry.source_mtime = metadata.source_mtime;
                entry.fingerprint =
                    job.task.id + "\n" + metadata.fingerprint;
            } catch (const std::exception& error) {
                failures.push_back({target.source_path, target.original_name,
                                    "read", error.what(), ""});
                continue;
            }
        }

        // 固定清单不按 max_files_per_run 截断；只让开其他在途作业的占用：
        // 明确选中的历史失败输入允许再次处理（不查 processed），
        // 被别的排队作业占着就记该目标失败并继续。
        if (queue_store_.is_file_in_flight(job.task.id, entry.fingerprint)) {
            // archive 目标被占用也必须保住原归档引用：丢了引用，
            // 下次重试会错把它当成定点补采去读源文件。
            failures.push_back({target.source_path, target.original_name,
                                "read",
                                "file is in flight by another pending job",
                                target.input_type == "archive"
                                    ? target.archive_raw_file_id
                                    : std::string{}});
            continue;
        }
        plan.push_back(std::move(entry));
    }
}

RecoveredJob TaskExecutor::run_collecting_stage(const RecoveredJob& input) const {
    RecoveredJob job = input;
    // 尚未生成计划的作业才选文件；file_failures 标记表示选文件已完成，
    // 零文件或全部失败的作业恢复时不再扫描、不重新 inspect 已判失败的目标。
    // 旧库作业没有标记，沿用“空计划即重扫”的老行为。
    if (job.files.empty() && !job.has_file_failures) {
        std::vector<PendingFilePlan> plan;
        std::vector<TaskRunReportFailedFile> failures;
        if (job.retry_files.empty()) {
            plan_from_directory(job, plan, failures);
        } else {
            plan_from_retry_targets(job, plan, failures);
        }
        queue_store_.save_file_plan(job.execution_key, plan, failures);
        job = load_job(job.execution_key);
    }

    // 兼容旧实现留下的半写状态：error_detail 已经落库、失败清单却漏了
    // 这条（旧版本两处分开写，中间重启就会这样）。能走到 collecting 说明
    // 报告还没保存过，按计划行补齐清单再继续；已发报告的作业不会进这里。
    if (!job.files.empty()) {
        auto amended = job.file_failures;
        bool amended_changed = false;
        for (const auto& file : job.files) {
            if (file.error_detail.empty()) {
                continue;
            }
            // 按源路径去重：清单里已有这条就只信已保存的版本。
            const auto recorded = std::any_of(
                amended.begin(), amended.end(),
                [&file](const TaskRunReportFailedFile& failed) {
                    return failed.source_path == file.source_path;
                });
            if (recorded) {
                continue;
            }
            amended.push_back({file.source_path, file.original_name,
                               "archive", file.error_detail,
                               inherited_archive_ref(job.retry_files,
                                                     file.source_path)});
            amended_changed = true;
        }
        if (amended_changed) {
            queue_store_.save_file_failures(job.execution_key, amended);
            job = load_job(job.execution_key);
        }
    }

    RawFileManifestRequest manifest;
    manifest.task_run_id = job.task_run_id;
    manifest.node_code = job.task.node_code;
    manifest.idempotency_key =
        make_manifest_idempotency_key(manifest.node_code, manifest.task_run_id);
    auto failures = job.file_failures;
    bool failed_this_run = false;
    for (const auto& file : job.files) {
        // 之前已判失败的目标（error_detail 非空）跳过，同一作业不重试它们。
        if (!file.error_detail.empty()) {
            continue;
        }
        LocalFileMetadata metadata{
            labbridge::core::fs::path{file.source_path},
            file.original_name,
            file.file_hash,
            file.size_bytes,
            file.source_mtime,
            file.fingerprint.substr(job.task.id.size() + 1),
        };
        try {
            archive_store_.recover_archive(metadata, file.archive_path,
                                           input_path_of(file));
        } catch (const ArchiveConflictError&) {
            // 归档证据冲突可能覆盖现场证据，整体转人工，不在文件级吞掉。
            throw;
        } catch (const std::exception& error) {
            // 单个文件归档失败：错误详情和更新后的完整失败清单一次落库，
            // 中途崩溃不会出现清单漏记；其他文件继续。
            // retry 重放读原归档失败时保留原引用，否则下次重试会错转补采。
            failures.push_back({file.source_path, file.original_name,
                                "archive", error.what(),
                                inherited_archive_ref(job.retry_files,
                                                      file.source_path)});
            queue_store_.mark_file_failed(job.execution_key, file.ordinal,
                                          error.what(), failures);
            failed_this_run = true;
            continue;
        }
        queue_store_.mark_file_archived(job.execution_key, file.ordinal);
        manifest.files.push_back({
            file.original_name, file.file_hash, file.archive_path,
            file.size_bytes, file.source_mtime, "archived_local"});
    }
    if (!manifest.files.empty()) {
        queue_store_.save_manifest(job.execution_key, manifest);
        job = load_job(job.execution_key);
    } else if (failed_this_run) {
        // 没有 manifest 可发也要重载：报告构建要拿到刚持久化的失败清单，
        // 否则会拿入参里的旧快照把 failed 报成 succeeded。
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
    // 运行级错误（目录缺失、配置非法）定位不到具体文件：
    // 显式传空清单表示“明确没有文件级失败”，与旧报告的 NULL 区分。
    report.has_failed_files = true;
    report.failed_files = current.has_file_failures
        ? current.file_failures
        : std::vector<TaskRunReportFailedFile>{};
    const std::vector<bool> parsed_without_errors(current.files.size(), false);
    queue_store_.save_report(
        current.execution_key, report, parsed_without_errors);
    return load_job(current.execution_key);
}

void TaskExecutor::run_reliable_job(RecoveredJob job) {
    if (job.stage == "start_pending") {
        const auto started = client_.start_task_run(job.start_request);
        if (started.run_status == "succeeded" ||
            started.run_status == "failed") {
            // 中心已把运行收尾（如开始前任务被停用）：
            // 本地只是还没开始的空作业，直接丢弃，不再采集、不发报告。
            queue_store_.discard_start_pending_job(job.execution_key);
            labbridge::core::log_warn(
                kComponent,
                "task run already finished in control plane; local job "
                "discarded; execution_key=" + job.execution_key +
                    "; run_status=" + started.run_status);
            return;
        }
        if (started.run_status != "running") {
            throw std::runtime_error(
                "unexpected run_status from start: " + started.run_status);
        }
        queue_store_.accept_start(job.execution_key, started.task_run_id);
        job = load_job(job.execution_key);
    }

    if (job.stage == "collecting") {
        try {
            job = run_collecting_stage(job);
        } catch (const ArchiveConflictError& error) {
            // 归档证据与持久化指纹不一致：自动重试可能覆盖现场证据，转人工处理。
            queue_store_.mark_requires_attention(
                job.execution_key, "archive_conflict", error.what());
            labbridge::core::log_warn(
                kComponent,
                "job requires attention; execution_key=" + job.execution_key +
                    "; reason=" + error.what());
            return;
        } catch (const AgentQueueError&) {
            // 队列库自身故障按 Phase 024 语义传播到进程边界。
            throw;
        } catch (const std::exception& error) {
            // 采集目录缺失、数据源配置非法等外部条件
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
        auto failures = job.file_failures;
        // 持久化的读取/归档失败先并入错误摘要，报告状态据此判 failed。
        for (const auto& failed : failures) {
            errors.add(failed.original_name + ": " + failed.message);
        }
        for (const auto& file : job.files) {
            // 从未取得有效归档的目标（读取/归档失败）不参与解析，
            // 文件结果按 false 覆盖。
            if (!file.error_detail.empty() || file.raw_file_id.empty()) {
                parsed_without_errors.push_back(false);
                continue;
            }
            const auto parsed = parser.parse({
                report.task_run_id, file.raw_file_id, file.archive_path});
            const bool clean = parsed.status.ok && parsed.errors.empty();
            parsed_without_errors.push_back(clean);
            if (!parsed.status.ok) {
                ++report.items_total;
                ++report.items_failed;
                errors.add(file.original_name + ": " + parsed.status.message);
                // 文件级解析错误：整个文件进入失败清单，归档引用指向本次 raw_file。
                failures.push_back({file.source_path, file.original_name,
                                    "parse", parsed.status.message,
                                    file.raw_file_id});
                continue;
            }
            report.items_total += static_cast<int>(
                parsed.records.size() + parsed.errors.size());
            report.items_success += static_cast<int>(parsed.records.size());
            report.items_failed += static_cast<int>(parsed.errors.size());
            for (const auto& error : parsed.errors) {
                errors.add(file.original_name + ": " + error);
            }
            if (!parsed.errors.empty()) {
                // 任一行解析错误即该文件失败；原因取第一条，保持简短。
                failures.push_back({file.source_path, file.original_name,
                                    "parse", parsed.errors.front(),
                                    file.raw_file_id});
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
        // 有文件级失败（含 QC 之外的解析失败）就终态 failed；
        // QC 不通过不进清单，也不影响这里的判定。
        report.status = (errors.empty() && failures.empty())
            ? labbridge::core::TaskRunStatus::Succeeded
            : labbridge::core::TaskRunStatus::Failed;
        report.finished_at = labbridge::core::format_utc_timestamp(now_());
        report.error_summary = errors.text();
        report.has_failed_files = true;
        report.failed_files = std::move(failures);
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
    queue_store_.begin_job(execution.task, request, {});
    run_reliable_job(load_job(request.execution_key));
}

PendingDispatchResult TaskExecutor::execute_pending(
    ManualTaskExecution execution) {
    if (stop_requested_.load(std::memory_order_acquire)) {
        // 停止后不接新候选；中心侧仍是 pending，下次启动继续。
        return PendingDispatchResult::QueueFull;
    }
    // 重复候选先按执行键查本地作业（含 retry_wait / attention）：
    // 已存在的不重新入队、不覆盖快照、不绕过已有退避或人工恢复。
    if (queue_store_.find_job(execution.execution.execution_key).has_value()) {
        return PendingDispatchResult::AlreadyQueued;
    }
    if (!queue_store_.has_capacity()) {
        // 人工请求留在中心 pending，本轮停止尝试，等正常唤醒再试。
        labbridge::core::log_warn(
            kComponent,
            "pending job capacity reached; keeping manual request pending in "
            "control plane; task_id=" + execution.task.id +
                "; execution_key=" + execution.execution.execution_key);
        return PendingDispatchResult::QueueFull;
    }
    StartTaskRunRequest request{
        execution.task.node_code,
        execution.task.id,
        execution.execution.execution_key,
        {},
        labbridge::core::format_utc_timestamp(now_()),
        execution.execution.trigger_type,
    };
    queue_store_.begin_job(execution.task, request,
                           execution.execution.retry_files);
    run_reliable_job(load_job(request.execution_key));
    return PendingDispatchResult::Dispatched;
}

void TaskExecutor::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    client_.request_stop();
}

}  // namespace labbridge::agent
