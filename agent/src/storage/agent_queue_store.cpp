#include "labbridge/agent/storage/agent_queue_store.h"

#include "labbridge/agent/execution/execution_request_codec.h"
#include "labbridge/core/filesystem.h"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

namespace labbridge::agent {
namespace {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

constexpr int kSchemaVersion = 2;
constexpr int kBusyTimeoutMilliseconds = 5000;
constexpr const char* kTimestampSql =
    "strftime('%Y-%m-%dT%H:%M:%fZ','now')";

constexpr const char* kSchema = R"SQL(
CREATE TABLE queue_metadata (
    singleton_id INTEGER PRIMARY KEY CHECK (singleton_id = 1),
    node_code TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE pending_jobs (
    execution_key TEXT PRIMARY KEY,
    node_code TEXT NOT NULL,
    task_id TEXT NOT NULL,
    scheduled_for TEXT NOT NULL,
    started_at TEXT NOT NULL,
    task_config_json TEXT NOT NULL,
    stage TEXT NOT NULL CHECK (stage IN (
        'start_pending',
        'collecting',
        'manifest_pending',
        'report_building',
        'report_pending',
        'retry_wait',
        'requires_attention'
    )),
    retry_stage TEXT,
    task_run_id TEXT,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    next_attempt_at TEXT,
    last_error_kind TEXT,
    last_error TEXT,
    retry_files_json TEXT,
    file_failures_json TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE pending_files (
    execution_key TEXT NOT NULL
        REFERENCES pending_jobs(execution_key) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL,
    source_path TEXT NOT NULL,
    original_name TEXT NOT NULL,
    source_mtime TEXT NOT NULL,
    size_bytes INTEGER NOT NULL CHECK (size_bytes >= 0),
    file_hash TEXT NOT NULL,
    fingerprint TEXT NOT NULL,
    archive_path TEXT NOT NULL,
    archive_state TEXT NOT NULL DEFAULT 'archive_planned',
    raw_file_id TEXT,
    parsed_without_errors INTEGER,
    error_detail TEXT,
    input_path TEXT,
    PRIMARY KEY (execution_key, ordinal),
    UNIQUE (execution_key, fingerprint)
);

CREATE TABLE pending_deliveries (
    execution_key TEXT NOT NULL
        REFERENCES pending_jobs(execution_key) ON DELETE CASCADE,
    request_type TEXT NOT NULL
        CHECK (request_type IN ('start', 'manifest', 'report')),
    idempotency_key TEXT NOT NULL,
    request_json TEXT NOT NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    next_attempt_at TEXT,
    last_error_kind TEXT,
    last_http_status INTEGER,
    last_error TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    PRIMARY KEY (execution_key, request_type),
    UNIQUE (request_type, idempotency_key)
);

CREATE TABLE delivery_attempts (
    id INTEGER PRIMARY KEY,
    execution_key TEXT NOT NULL
        REFERENCES pending_jobs(execution_key) ON DELETE CASCADE,
    request_type TEXT NOT NULL,
    attempt_number INTEGER NOT NULL,
    attempted_at TEXT NOT NULL,
    outcome TEXT NOT NULL CHECK (outcome IN (
        'success',
        'retryable_failure',
        'permanent_failure'
    )),
    error_kind TEXT,
    http_status INTEGER,
    message TEXT
);

CREATE TABLE processed_files (
    task_id TEXT NOT NULL,
    fingerprint TEXT NOT NULL,
    source_path TEXT NOT NULL,
    file_hash TEXT NOT NULL,
    processed_at TEXT NOT NULL,
    execution_key TEXT NOT NULL,
    PRIMARY KEY (task_id, fingerprint)
);

CREATE INDEX pending_jobs_due_idx
    ON pending_jobs(stage, next_attempt_at, created_at);
CREATE INDEX pending_files_fingerprint_idx
    ON pending_files(fingerprint);
CREATE INDEX processed_files_task_time_idx
    ON processed_files(task_id, processed_at);
)SQL";

std::string format_sqlite_error(int result,
                                sqlite3* database,
                                const char* raw_message) {
    const char* detail =
        raw_message != nullptr ? raw_message : sqlite3_errmsg(database);
    return "sqlite code=" + std::to_string(result) + " " + detail;
}

void check_result(int result,
                  sqlite3* database,
                  const std::string& operation) {
    if (result == SQLITE_OK || result == SQLITE_DONE || result == SQLITE_ROW) {
        return;
    }

    throw AgentQueueError(
        operation + " failed: " + format_sqlite_error(result, database, nullptr));
}

void execute(sqlite3* database,
             const char* sql,
             const std::string& operation) {
    char* raw_message = nullptr;
    const int result =
        sqlite3_exec(database, sql, nullptr, nullptr, &raw_message);
    if (result == SQLITE_OK) {
        return;
    }

    const std::string detail =
        format_sqlite_error(result, database, raw_message);
    sqlite3_free(raw_message);
    throw AgentQueueError(operation + " failed: " + detail);
}

Statement prepare(sqlite3* database,
                  const std::string& sql,
                  const std::string& operation) {
    sqlite3_stmt* raw_statement = nullptr;
    check_result(
        sqlite3_prepare_v2(database, sql.c_str(), -1, &raw_statement, nullptr),
        database,
        operation);
    return Statement{raw_statement, sqlite3_finalize};
}

void bind_text(sqlite3* database,
               sqlite3_stmt* statement,
               int index,
               const std::string& value,
               const std::string& operation) {
    check_result(
        sqlite3_bind_text(statement,
                          index,
                          value.c_str(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT),
        database,
        operation);
}

std::string read_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    if (value == nullptr) {
        return {};
    }
    return reinterpret_cast<const char*>(value);
}

class Transaction final {
public:
    Transaction(sqlite3* database, const std::string& operation)
        : database_(database), operation_(operation) {
        execute(database_, "BEGIN IMMEDIATE", "begin " + operation_);
    }

    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        execute(database_, "COMMIT", "commit " + operation_);
        committed_ = true;
    }

private:
    sqlite3* database_;
    std::string operation_;
    bool committed_{false};
};

int read_schema_version(sqlite3* database) {
    auto statement =
        prepare(database, "PRAGMA user_version", "read schema version");
    check_result(
        sqlite3_step(statement.get()), database, "read schema version");
    return sqlite3_column_int(statement.get(), 0);
}

void initialize_schema(sqlite3* database, const std::string& node_code) {
    Transaction transaction{database, "schema initialization"};
    execute(database, kSchema, "create queue schema");

    const std::string insert_metadata =
        "INSERT INTO queue_metadata "
        "(singleton_id, node_code, created_at, updated_at) VALUES "
        "(1, ?, " +
        std::string{kTimestampSql} + ", " + kTimestampSql + ")";
    auto statement =
        prepare(database, insert_metadata.c_str(), "bind node identity");
    bind_text(database, statement.get(), 1, node_code, "bind node identity");
    check_result(
        sqlite3_step(statement.get()), database, "bind node identity");

    execute(database, "PRAGMA user_version=2", "set schema version");
    transaction.commit();
}

// v1 → v2 只扩列不加表，一个事务内完成并更新版本号；
// 只由运行时构造器（持锁）调用，维护命令不迁移。
void migrate_schema_v1_to_v2(sqlite3* database) {
    Transaction transaction{database, "schema migration v1 to v2"};
    execute(database,
            "ALTER TABLE pending_jobs ADD COLUMN retry_files_json TEXT",
            "add retry_files_json");
    execute(database,
            "ALTER TABLE pending_jobs ADD COLUMN file_failures_json TEXT",
            "add file_failures_json");
    execute(database,
            "ALTER TABLE pending_files ADD COLUMN input_path TEXT",
            "add input_path");
    execute(database, "PRAGMA user_version=2", "set schema version");
    transaction.commit();
}

void validate_node_identity(sqlite3* database,
                            const std::string& node_code) {
    auto statement = prepare(
        database,
        "SELECT node_code FROM queue_metadata WHERE singleton_id = 1",
        "validate node identity");
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW || read_text(statement.get(), 0) != node_code) {
        throw AgentQueueError("queue database node identity mismatch");
    }
}

void validate_required_tables(sqlite3* database) {
    constexpr std::array<const char*, 5> required_tables{
        "pending_jobs",
        "pending_files",
        "pending_deliveries",
        "delivery_attempts",
        "processed_files",
    };

    for (const char* table : required_tables) {
        auto statement = prepare(
            database,
            "SELECT count(*) FROM sqlite_master "
            "WHERE type = 'table' AND name = ?",
            "validate queue schema");
        bind_text(
            database, statement.get(), 1, table, "validate queue schema");

        if (sqlite3_step(statement.get()) != SQLITE_ROW ||
            sqlite3_column_int(statement.get(), 0) != 1) {
            throw AgentQueueError("queue schema is incomplete");
        }
    }
}

std::size_t read_pending_job_count(sqlite3* database) {
    auto statement = prepare(
        database, "SELECT count(*) FROM pending_jobs", "count pending jobs");
    check_result(
        sqlite3_step(statement.get()), database, "count pending jobs");
    return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
}

// 可执行阶段白名单：retry_stage 只允许指向这些阶段。
// retry_wait / requires_attention 本身不可执行，不能作为恢复位置。
bool is_executable_stage(const std::string& stage) {
    return stage == "start_pending" || stage == "collecting" ||
           stage == "manifest_pending" || stage == "report_building" ||
           stage == "report_pending";
}

void insert_pending_job(sqlite3* database,
                        const StartTaskRunRequest& request,
                        const std::string& task_json,
                        const std::string& retry_files_json) {
    auto statement = prepare(
        database,
        "INSERT INTO pending_jobs "
        "(execution_key, node_code, task_id, scheduled_for, started_at, "
        "task_config_json, retry_files_json, stage, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 'start_pending', "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        "insert pending job");

    const std::array<const std::string*, 7> values{
        &request.execution_key,
        &request.node_code,
        &request.task_id,
        &request.scheduled_for,
        &request.started_at,
        &task_json,
        &retry_files_json,
    };
    for (std::size_t index = 0; index < values.size(); ++index) {
        bind_text(database,
                  statement.get(),
                  static_cast<int>(index + 1),
                  *values[index],
                  "insert pending job");
    }
    check_result(
        sqlite3_step(statement.get()), database, "insert pending job");
}

void insert_start_delivery(sqlite3* database,
                           const StartTaskRunRequest& request,
                           const std::string& request_json) {
    auto statement = prepare(
        database,
        "INSERT INTO pending_deliveries "
        "(execution_key, request_type, idempotency_key, request_json, "
        "created_at, updated_at) VALUES "
        "(?, 'start', ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        "insert start delivery");
    bind_text(database,
              statement.get(),
              1,
              request.execution_key,
              "insert start delivery");
    bind_text(database,
              statement.get(),
              2,
              request.execution_key,
              "insert start delivery");
    bind_text(database,
              statement.get(),
              3,
              request_json,
              "insert start delivery");
    check_result(
        sqlite3_step(statement.get()), database, "insert start delivery");
}

void insert_pending_file(sqlite3* database,
                         const std::string& execution_key,
                         const PendingFilePlan& file) {
    auto statement = prepare(
        database,
        "INSERT INTO pending_files "
        "(execution_key, ordinal, source_path, original_name, source_mtime, "
        "size_bytes, file_hash, fingerprint, archive_path, input_path) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        "insert file plan");

    bind_text(database,
              statement.get(),
              1,
              execution_key,
              "insert file plan");
    check_result(
        sqlite3_bind_int(statement.get(), 2, file.ordinal),
        database,
        "insert file plan");
    bind_text(
        database, statement.get(), 3, file.source_path, "insert file plan");
    bind_text(database,
              statement.get(),
              4,
              file.original_name,
              "insert file plan");
    bind_text(database,
              statement.get(),
              5,
              file.source_mtime,
              "insert file plan");
    check_result(
        sqlite3_bind_int64(statement.get(), 6, file.size_bytes),
        database,
        "insert file plan");
    bind_text(
        database, statement.get(), 7, file.file_hash, "insert file plan");
    bind_text(
        database, statement.get(), 8, file.fingerprint, "insert file plan");
    bind_text(database,
              statement.get(),
              9,
              file.archive_path,
              "insert file plan");
    bind_text(
        database, statement.get(), 10, file.input_path, "insert file plan");
    check_result(
        sqlite3_step(statement.get()), database, "insert file plan");
}

std::vector<PendingFilePlan> recover_file_plan(
    sqlite3* database,
    const std::string& execution_key) {
    auto statement = prepare(
        database,
        "SELECT ordinal, source_path, original_name, source_mtime, "
        "size_bytes, file_hash, fingerprint, archive_path, archive_state, "
        "COALESCE(raw_file_id, ''), COALESCE(parsed_without_errors, 0), "
        "COALESCE(input_path, ''), COALESCE(error_detail, '') "
        "FROM pending_files WHERE execution_key = ? ORDER BY ordinal",
        "recover file plan");
    bind_text(database,
              statement.get(),
              1,
              execution_key,
              "recover file plan");

    std::vector<PendingFilePlan> files;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        files.push_back({
            sqlite3_column_int(statement.get(), 0),
            read_text(statement.get(), 1),
            read_text(statement.get(), 2),
            read_text(statement.get(), 3),
            sqlite3_column_int64(statement.get(), 4),
            read_text(statement.get(), 5),
            read_text(statement.get(), 6),
            read_text(statement.get(), 7),
            read_text(statement.get(), 8),
            read_text(statement.get(), 9),
            sqlite3_column_int(statement.get(), 10) == 1,
            read_text(statement.get(), 11),
            read_text(statement.get(), 12),
        });
    }
    return files;
}

// 持久化的失败清单格式与报告 DTO 相同；定义在下方，先声明给行映射用。
std::vector<TaskRunReportFailedFile> decode_failed_files(
    const std::string& json);

// recover_jobs 与 load_job 共用的作业行映射；列顺序由调用方 SQL 保证。
RecoveredJob read_recovered_job(sqlite3* database, sqlite3_stmt* statement) {
    RecoveredJob job;
    job.execution_key = read_text(statement, 0);
    job.task = decode_task_config(read_text(statement, 1));
    job.stage = read_text(statement, 2);
    job.start_request =
        decode_start_task_run_request(read_text(statement, 3));
    job.task_run_id = read_text(statement, 4);
    const auto manifest_json = read_text(statement, 5);
    if (!manifest_json.empty()) {
        job.manifest_request =
            decode_raw_file_manifest_request(manifest_json);
    }
    const auto report_json = read_text(statement, 6);
    if (!report_json.empty()) {
        job.report_request =
            decode_task_run_report_request(report_json);
    }
    job.retry_files = decode_retry_files(read_text(statement, 7));
    const auto failures_json = read_text(statement, 8);
    if (!failures_json.empty()) {
        job.has_file_failures = true;
        const auto persisted = decode_failed_files(failures_json);
        job.file_failures = std::move(persisted);
    }
    job.files = recover_file_plan(database, job.execution_key);
    return job;
}

// 持久化的失败清单格式与报告 DTO 相同，直接复用报告编解码。
std::vector<TaskRunReportFailedFile> decode_failed_files(
    const std::string& json) {
    std::vector<TaskRunReportFailedFile> failures;
    if (json.empty()) {
        return failures;
    }
    try {
        const auto payload = nlohmann::json::parse(json);
        if (!payload.is_array()) {
            throw AgentQueueError("file failures must be an array");
        }
        for (const auto& item : payload) {
            failures.push_back(TaskRunReportFailedFile{
                item.at("source_path").get<std::string>(),
                item.at("original_name").get<std::string>(),
                item.at("stage").get<std::string>(),
                item.value("message", std::string{}),
                item.value("archive_raw_file_id", std::string{}),
            });
        }
    } catch (const nlohmann::json::exception& error) {
        throw AgentQueueError(
            std::string{"invalid persisted file failures: "} + error.what());
    }
    return failures;
}

std::string encode_file_failures(
    const std::vector<TaskRunReportFailedFile>& failures) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& failed : failures) {
        array.push_back(nlohmann::json{
            {"source_path", failed.source_path},
            {"original_name", failed.original_name},
            {"stage", failed.stage},
            {"message", failed.message},
            {"archive_raw_file_id", failed.archive_raw_file_id},
        });
    }
    return array.dump();
}

constexpr const char* kRecoveredJobColumns =
    "jobs.execution_key, jobs.task_config_json, "
    "CASE WHEN jobs.stage = 'retry_wait' THEN jobs.retry_stage ELSE jobs.stage END, "
    "deliveries.request_json, COALESCE(jobs.task_run_id, ''), "
    "COALESCE((SELECT request_json FROM pending_deliveries "
    "WHERE execution_key = jobs.execution_key AND request_type = 'manifest'), ''), "
    "COALESCE((SELECT request_json FROM pending_deliveries "
    "WHERE execution_key = jobs.execution_key AND request_type = 'report'), ''), "
    "COALESCE(jobs.retry_files_json, ''), "
    "COALESCE(jobs.file_failures_json, '') ";

std::vector<AttentionFileDetail> read_attention_files(
    sqlite3* database,
    const std::string& execution_key) {
    auto statement = prepare(
        database,
        "SELECT ordinal, source_path, size_bytes, archive_path, archive_state "
        "FROM pending_files WHERE execution_key = ? ORDER BY ordinal",
        "load attention files");
    bind_text(database,
              statement.get(),
              1,
              execution_key,
              "load attention files");

    std::vector<AttentionFileDetail> files;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        files.push_back({
            sqlite3_column_int(statement.get(), 0),
            read_text(statement.get(), 1),
            sqlite3_column_int64(statement.get(), 2),
            read_text(statement.get(), 3),
            read_text(statement.get(), 4),
        });
    }
    return files;
}

// 持久化请求还原成实际发送的 HTTP body 大小：解码后走与投递层
// 相同的 encode_*_http_body，容量排查看到的字节数和发送检查一致。
// request_type 受表上 CHECK 约束限制，只可能是三种。
long long http_body_bytes(const std::string& request_type,
                          const std::string& request_json) {
    if (request_type == "start") {
        return static_cast<long long>(
            encode_start_task_run_http_body(
                decode_start_task_run_request(request_json))
                .size());
    }
    if (request_type == "manifest") {
        return static_cast<long long>(
            encode_raw_file_manifest_http_body(
                decode_raw_file_manifest_request(request_json))
                .size());
    }
    return static_cast<long long>(
        encode_task_run_report_http_body(
            decode_task_run_report_request(request_json))
            .size());
}

std::vector<AttentionDeliveryDetail> read_attention_deliveries(
    sqlite3* database,
    const std::string& execution_key) {
    auto statement = prepare(
        database,
        "SELECT request_type, attempt_count, COALESCE(next_attempt_at, ''), "
        "COALESCE(last_http_status, 0), COALESCE(last_error_kind, ''), "
        "COALESCE(last_error, ''), request_json "
        "FROM pending_deliveries WHERE execution_key = ? "
        "ORDER BY CASE request_type "
        "WHEN 'start' THEN 1 WHEN 'manifest' THEN 2 ELSE 3 END",
        "load attention deliveries");
    bind_text(database,
              statement.get(),
              1,
              execution_key,
              "load attention deliveries");

    std::vector<AttentionDeliveryDetail> deliveries;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        AttentionDeliveryDetail delivery;
        delivery.request_type = read_text(statement.get(), 0);
        delivery.attempt_count = sqlite3_column_int(statement.get(), 1);
        delivery.next_attempt_at = read_text(statement.get(), 2);
        delivery.last_http_status = sqlite3_column_int(statement.get(), 3);
        delivery.last_error_kind = read_text(statement.get(), 4);
        delivery.last_error = read_text(statement.get(), 5);
        delivery.body_bytes =
            http_body_bytes(delivery.request_type,
                            read_text(statement.get(), 6));
        deliveries.push_back(std::move(delivery));
    }
    return deliveries;
}

}  // namespace

struct AgentQueueStore::Impl {
    sqlite3* database{nullptr};
    std::string node_code;
    std::size_t max_pending_jobs{0};
    std::size_t processed_fingerprint_capacity{0};
    mutable std::mutex mutex;

    ~Impl() {
        if (database != nullptr) {
            sqlite3_close_v2(database);
        }
    }
};

AgentQueueStore::AgentQueueStore(std::string database_path,
                                 std::string node_code,
                                 std::size_t max_pending_jobs,
                                 std::size_t processed_fingerprint_capacity)
    : impl_(std::make_unique<Impl>()) {
    if (database_path.empty() || node_code.empty() || max_pending_jobs == 0 ||
        processed_fingerprint_capacity == 0) {
        throw AgentQueueError(
            "database path, node identity and capacity are required");
    }

    impl_->node_code = std::move(node_code);
    impl_->max_pending_jobs = max_pending_jobs;
    impl_->processed_fingerprint_capacity = processed_fingerprint_capacity;

    const labbridge::core::fs::path path{database_path};
    if (!path.parent_path().empty()) {
        labbridge::core::fs::create_directories(path.parent_path());
    }

    const int open_result = sqlite3_open_v2(
        database_path.c_str(),
        &impl_->database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr);
    if (open_result != SQLITE_OK) {
        const std::string detail = impl_->database == nullptr
            ? "unknown SQLite open error"
            : sqlite3_errmsg(impl_->database);
        throw AgentQueueError("open queue database failed: " + detail);
    }

    sqlite3_busy_timeout(impl_->database, kBusyTimeoutMilliseconds);
    execute(impl_->database,
            "PRAGMA foreign_keys=ON; "
            "PRAGMA journal_mode=WAL; "
            "PRAGMA synchronous=FULL;",
            "configure queue database");

    const int schema_version = read_schema_version(impl_->database);
    if (schema_version > kSchemaVersion) {
        throw AgentQueueError(
            "unsupported queue schema version " +
            std::to_string(schema_version));
    }
    if (schema_version == 0) {
        initialize_schema(impl_->database, impl_->node_code);
    } else if (schema_version == 1) {
        // 迁移只发生在运行时构造器：main 已先取得队列独占锁，
        // 这里加列、升版本一个事务完成，旧作业和原请求全保留。
        migrate_schema_v1_to_v2(impl_->database);
    }

    // 已有队列必须通过身份和结构校验，禁止静默重建丢失待投递证据。
    validate_node_identity(impl_->database, impl_->node_code);
    validate_required_tables(impl_->database);
}

AgentQueueStore::~AgentQueueStore() = default;

std::size_t AgentQueueStore::pending_job_count() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return read_pending_job_count(impl_->database);
}

bool AgentQueueStore::begin_job(
    const labbridge::core::TaskConfig& task,
    const StartTaskRunRequest& request,
    const std::vector<RetryFileInput>& retry_files) {
    if (request.node_code != impl_->node_code ||
        task.node_code != impl_->node_code || request.task_id != task.id) {
        throw AgentQueueError("job identity does not match queue identity");
    }

    const auto task_json = encode_task_config(task);
    const auto request_json = encode_start_task_run_request(request);
    // 固定重试目标与作业、start 投递同事务保存，之后不再改写。
    const auto retry_files_json =
        retry_files.empty() ? std::string{} : encode_retry_files(retry_files);

    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "job transaction"};

    auto existing = prepare(
        impl_->database,
        "SELECT task_id, scheduled_for, task_config_json "
        "FROM pending_jobs WHERE execution_key = ?",
        "find pending job");
    bind_text(impl_->database,
              existing.get(),
              1,
              request.execution_key,
              "find pending job");

    if (sqlite3_step(existing.get()) == SQLITE_ROW) {
        const bool same_job =
            read_text(existing.get(), 0) == task.id &&
            read_text(existing.get(), 1) == request.scheduled_for &&
            read_text(existing.get(), 2) == task_json;
        if (!same_job) {
            throw AgentQueueError(
                "execution key conflicts with persisted job");
        }

        transaction.commit();
        return false;
    }

    if (read_pending_job_count(impl_->database) >=
        impl_->max_pending_jobs) {
        throw AgentQueueError("pending job capacity reached");
    }

    // job 与首次 start delivery 必须一起提交，避免留下不可重放的半成品。
    insert_pending_job(impl_->database, request, task_json, retry_files_json);
    insert_start_delivery(impl_->database, request, request_json);
    transaction.commit();
    return true;
}

void AgentQueueStore::save_file_plan(
    const std::string& execution_key,
    const std::vector<PendingFilePlan>& files,
    const std::vector<TaskRunReportFailedFile>& failures) {
    const auto failures_json = encode_file_failures(failures);
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "file plan"};

    // 完整文件计划与“选文件已完成”的失败标记按批次提交：
    // 任何一条约束失败都不能留下部分计划，也不能出现有计划没标记的中间态。
    for (const auto& file : files) {
        insert_pending_file(impl_->database, execution_key, file);
    }
    auto marker = prepare(
        impl_->database,
        "UPDATE pending_jobs SET file_failures_json = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ?",
        "mark file selection");
    bind_text(impl_->database, marker.get(), 1, failures_json,
              "mark file selection");
    bind_text(impl_->database, marker.get(), 2, execution_key,
              "mark file selection");
    check_result(sqlite3_step(marker.get()), impl_->database,
                 "mark file selection");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError("file plan requires a pending job");
    }
    transaction.commit();
}

std::vector<RecoveredJob> AgentQueueStore::recover_jobs() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        std::string{"SELECT "} + kRecoveredJobColumns +
        "FROM pending_jobs AS jobs "
        "JOIN pending_deliveries AS deliveries "
        "ON deliveries.execution_key = jobs.execution_key "
        "AND deliveries.request_type = 'start' WHERE jobs.stage != 'requires_attention' "
        "ORDER BY jobs.created_at, jobs.execution_key",
        "recover jobs");

    std::vector<RecoveredJob> jobs;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        jobs.push_back(read_recovered_job(impl_->database, statement.get()));
    }
    return jobs;
}

RecoveredJob AgentQueueStore::load_job(
    const std::string& execution_key) const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        std::string{"SELECT "} + kRecoveredJobColumns +
        "FROM pending_jobs AS jobs "
        "JOIN pending_deliveries AS deliveries "
        "ON deliveries.execution_key = jobs.execution_key "
        "AND deliveries.request_type = 'start' "
        "WHERE jobs.execution_key = ? AND jobs.stage != 'requires_attention'",
        "load job");
    bind_text(impl_->database,
              statement.get(),
              1,
              execution_key,
              "load job");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw AgentQueueError("pending job does not exist: " + execution_key);
    }
    return read_recovered_job(impl_->database, statement.get());
}

std::optional<RecoveredJob> AgentQueueStore::find_job(
    const std::string& execution_key) const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    // 人工候选去重要能看到所有形态的作业，包括 requires_attention；
    // 已存在的执行键不重新入队、不覆盖快照、不绕过人工恢复。
    auto statement = prepare(
        impl_->database,
        std::string{"SELECT "} + kRecoveredJobColumns +
        "FROM pending_jobs AS jobs "
        "JOIN pending_deliveries AS deliveries "
        "ON deliveries.execution_key = jobs.execution_key "
        "AND deliveries.request_type = 'start' "
        "WHERE jobs.execution_key = ?",
        "find job");
    bind_text(impl_->database,
              statement.get(),
              1,
              execution_key,
              "find job");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    return read_recovered_job(impl_->database, statement.get());
}

void AgentQueueStore::save_file_failures(
    const std::string& execution_key,
    const std::vector<TaskRunReportFailedFile>& failures) {
    const auto failures_json = encode_file_failures(failures);
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "save file failures"};
    auto statement = prepare(
        impl_->database,
        "UPDATE pending_jobs SET file_failures_json = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ?",
        "save file failures");
    bind_text(impl_->database, statement.get(), 1, failures_json,
              "save file failures");
    bind_text(impl_->database, statement.get(), 2, execution_key,
              "save file failures");
    check_result(sqlite3_step(statement.get()), impl_->database,
                 "save file failures");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError("save file failures requires a pending job");
    }
    transaction.commit();
}

void AgentQueueStore::mark_file_failed(const std::string& execution_key,
                                       int ordinal,
                                       const std::string& error_detail) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        "UPDATE pending_files SET error_detail = ? "
        "WHERE execution_key = ? AND ordinal = ?",
        "mark file failed");
    bind_text(impl_->database, statement.get(), 1, error_detail,
              "mark file failed");
    bind_text(impl_->database, statement.get(), 2, execution_key,
              "mark file failed");
    check_result(sqlite3_bind_int(statement.get(), 3, ordinal),
                 impl_->database, "mark file failed");
    check_result(sqlite3_step(statement.get()), impl_->database,
                 "mark file failed");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError("pending file does not exist");
    }
}

void AgentQueueStore::accept_start(const std::string& execution_key,
                                   const std::string& task_run_id) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "accept start"};
    auto statement = prepare(
        impl_->database,
        "UPDATE pending_jobs SET task_run_id = ?, stage = 'collecting', "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ? AND stage = 'start_pending'",
        "accept start");
    bind_text(impl_->database, statement.get(), 1, task_run_id, "accept start");
    bind_text(impl_->database, statement.get(), 2, execution_key, "accept start");
    check_result(sqlite3_step(statement.get()), impl_->database, "accept start");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError("accept start requires start_pending job");
    }
    transaction.commit();
}

void AgentQueueStore::mark_file_archived(const std::string& execution_key,
                                         int ordinal) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        "UPDATE pending_files SET archive_state = 'archived' "
        "WHERE execution_key = ? AND ordinal = ?",
        "mark file archived");
    bind_text(impl_->database, statement.get(), 1, execution_key,
              "mark file archived");
    check_result(sqlite3_bind_int(statement.get(), 2, ordinal),
                 impl_->database, "mark file archived");
    check_result(sqlite3_step(statement.get()), impl_->database,
                 "mark file archived");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError("pending file does not exist");
    }
}

void AgentQueueStore::save_manifest(
    const std::string& execution_key,
    const RawFileManifestRequest& request) {
    const auto request_json = encode_raw_file_manifest_request(request);
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "save manifest"};
    auto delivery = prepare(
        impl_->database,
        "INSERT INTO pending_deliveries "
        "(execution_key, request_type, idempotency_key, request_json, "
        "created_at, updated_at) VALUES (?, 'manifest', ?, ?, "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        "save manifest");
    bind_text(impl_->database, delivery.get(), 1, execution_key, "save manifest");
    bind_text(impl_->database, delivery.get(), 2, request.idempotency_key,
              "save manifest");
    bind_text(impl_->database, delivery.get(), 3, request_json, "save manifest");
    check_result(sqlite3_step(delivery.get()), impl_->database, "save manifest");
    auto job = prepare(
        impl_->database,
        "UPDATE pending_jobs SET stage = 'manifest_pending', "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ? AND stage = 'collecting'",
        "advance manifest");
    bind_text(impl_->database, job.get(), 1, execution_key, "advance manifest");
    check_result(sqlite3_step(job.get()), impl_->database, "advance manifest");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError("save manifest requires collecting job");
    }
    transaction.commit();
}

void AgentQueueStore::accept_manifest(
    const std::string& execution_key,
    const std::vector<std::string>& raw_file_ids) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "accept manifest"};
    // manifest 只包含已归档项，raw_file_ids 必须按实际发送的条目顺序绑定：
    // 按持久化 manifest 的 storage_path（本次新归档路径，run 内唯一）对号，
    // 不能按全部 pending_files 数量和连续下标匹配——失败目标不进 manifest。
    auto persisted = prepare(
        impl_->database,
        "SELECT request_json FROM pending_deliveries "
        "WHERE execution_key = ? AND request_type = 'manifest'",
        "load persisted manifest");
    bind_text(impl_->database, persisted.get(), 1, execution_key,
              "load persisted manifest");
    if (sqlite3_step(persisted.get()) != SQLITE_ROW) {
        throw AgentQueueError("persisted manifest does not exist");
    }
    const auto manifest_request = decode_raw_file_manifest_request(
        read_text(persisted.get(), 0));
    if (manifest_request.files.size() != raw_file_ids.size()) {
        throw AgentQueueError("manifest raw ID count mismatch");
    }
    auto update = prepare(
        impl_->database,
        "UPDATE pending_files SET raw_file_id = ? "
        "WHERE execution_key = ? AND archive_path = ?",
        "map raw file ID");
    for (std::size_t index = 0; index < raw_file_ids.size(); ++index) {
        sqlite3_reset(update.get());
        sqlite3_clear_bindings(update.get());
        bind_text(impl_->database, update.get(), 1, raw_file_ids[index],
                  "map raw file ID");
        bind_text(impl_->database, update.get(), 2, execution_key,
                  "map raw file ID");
        bind_text(impl_->database, update.get(), 3,
                  manifest_request.files[index].storage_path,
                  "map raw file ID");
        check_result(sqlite3_step(update.get()), impl_->database,
                     "map raw file ID");
        if (sqlite3_changes(impl_->database) != 1) {
            throw AgentQueueError("manifest file does not exist");
        }
    }
    auto job = prepare(
        impl_->database,
        "UPDATE pending_jobs SET stage = 'report_building', "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ? AND stage = 'manifest_pending'",
        "advance report building");
    bind_text(impl_->database, job.get(), 1, execution_key,
              "advance report building");
    check_result(sqlite3_step(job.get()), impl_->database,
                  "advance report building");
    transaction.commit();
}

void AgentQueueStore::save_report(
    const std::string& execution_key,
    const TaskRunReportRequest& request,
    const std::vector<bool>& parsed_without_errors) {
    const auto request_json = encode_task_run_report_request(request);
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "save report"};
    auto file_count = prepare(
        impl_->database,
        "SELECT count(*) FROM pending_files WHERE execution_key = ?",
        "count report files");
    bind_text(impl_->database, file_count.get(), 1, execution_key,
              "count report files");
    check_result(sqlite3_step(file_count.get()), impl_->database,
                 "count report files");
    if (sqlite3_column_int64(file_count.get(), 0) !=
        static_cast<sqlite3_int64>(parsed_without_errors.size())) {
        throw AgentQueueError("report file outcome count mismatch");
    }
    auto delivery = prepare(
        impl_->database,
        "INSERT INTO pending_deliveries "
        "(execution_key, request_type, idempotency_key, request_json, "
        "created_at, updated_at) VALUES (?, 'report', ?, ?, "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        "save report");
    bind_text(impl_->database, delivery.get(), 1, execution_key, "save report");
    bind_text(impl_->database, delivery.get(), 2, request.idempotency_key,
              "save report");
    bind_text(impl_->database, delivery.get(), 3, request_json, "save report");
    check_result(sqlite3_step(delivery.get()), impl_->database, "save report");
    auto file = prepare(
        impl_->database,
        "UPDATE pending_files SET parsed_without_errors = ? "
        "WHERE execution_key = ? AND ordinal = ?",
        "save parsed outcome");
    for (std::size_t index = 0; index < parsed_without_errors.size(); ++index) {
        sqlite3_reset(file.get());
        sqlite3_clear_bindings(file.get());
        check_result(sqlite3_bind_int(file.get(), 1,
                                     parsed_without_errors[index] ? 1 : 0),
                     impl_->database, "save parsed outcome");
        bind_text(impl_->database, file.get(), 2, execution_key,
                  "save parsed outcome");
        check_result(sqlite3_bind_int(file.get(), 3, static_cast<int>(index)),
                     impl_->database, "save parsed outcome");
        check_result(sqlite3_step(file.get()), impl_->database,
                     "save parsed outcome");
    }
    auto job = prepare(
        impl_->database,
        "UPDATE pending_jobs SET stage = 'report_pending', "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ? AND stage IN ('collecting','report_building')",
        "advance report pending");
    bind_text(impl_->database, job.get(), 1, execution_key,
              "advance report pending");
    check_result(sqlite3_step(job.get()), impl_->database,
                 "advance report pending");
    transaction.commit();
}

void AgentQueueStore::complete_job(const std::string& execution_key) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "complete job"};
    auto insert = prepare(
        impl_->database,
        "INSERT OR IGNORE INTO processed_files "
        "(task_id, fingerprint, source_path, file_hash, processed_at, execution_key) "
        "SELECT jobs.task_id, files.fingerprint, files.source_path, "
        "files.file_hash, strftime('%Y-%m-%dT%H:%M:%fZ','now'), jobs.execution_key "
        "FROM pending_jobs jobs JOIN pending_files files "
        "ON files.execution_key = jobs.execution_key "
        "WHERE jobs.execution_key = ? AND jobs.stage = 'report_pending' "
        "AND files.parsed_without_errors = 1",
        "write processed fingerprints");
    bind_text(impl_->database, insert.get(), 1, execution_key,
              "write processed fingerprints");
    check_result(sqlite3_step(insert.get()), impl_->database,
                 "write processed fingerprints");
    auto trim = prepare(
        impl_->database,
        "DELETE FROM processed_files WHERE task_id = "
        "(SELECT task_id FROM pending_jobs WHERE execution_key = ?) "
        "AND rowid NOT IN (SELECT rowid FROM processed_files WHERE task_id = "
        "(SELECT task_id FROM pending_jobs WHERE execution_key = ?) "
        "ORDER BY processed_at DESC, rowid DESC LIMIT ?)",
        "trim processed fingerprints");
    bind_text(impl_->database, trim.get(), 1, execution_key,
              "trim processed fingerprints");
    bind_text(impl_->database, trim.get(), 2, execution_key,
              "trim processed fingerprints");
    check_result(sqlite3_bind_int64(
                     trim.get(), 3,
                     static_cast<sqlite3_int64>(
                         impl_->processed_fingerprint_capacity)),
                 impl_->database, "trim processed fingerprints");
    check_result(sqlite3_step(trim.get()), impl_->database,
                 "trim processed fingerprints");
    auto remove = prepare(
        impl_->database,
        "DELETE FROM pending_jobs WHERE execution_key = ? "
        "AND stage = 'report_pending'",
        "complete job");
    bind_text(impl_->database, remove.get(), 1, execution_key, "complete job");
    check_result(sqlite3_step(remove.get()), impl_->database, "complete job");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError("complete job requires report_pending job");
    }
    transaction.commit();
}

void AgentQueueStore::discard_start_pending_job(
    const std::string& execution_key) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "discard start pending job"};
    // 只收尾“中心已终态、本地还没生成文件计划”的空作业：
    // 有计划行说明已开始采集，不能走这个入口。
    auto count = prepare(
        impl_->database,
        "SELECT count(*) FROM pending_files WHERE execution_key = ?",
        "count discard files");
    bind_text(impl_->database, count.get(), 1, execution_key,
              "count discard files");
    check_result(sqlite3_step(count.get()), impl_->database,
                 "count discard files");
    if (sqlite3_column_int64(count.get(), 0) != 0) {
        throw AgentQueueError(
            "discard requires a job without a file plan");
    }
    auto remove = prepare(
        impl_->database,
        "DELETE FROM pending_jobs WHERE execution_key = ? "
        "AND stage = 'start_pending'",
        "discard start pending job");
    bind_text(impl_->database, remove.get(), 1, execution_key,
              "discard start pending job");
    check_result(sqlite3_step(remove.get()), impl_->database,
                 "discard start pending job");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError(
            "discard requires a start_pending job: " + execution_key);
    }
    transaction.commit();
}

void AgentQueueStore::mark_requires_attention(
    const std::string& execution_key,
    const std::string& error_kind,
    const std::string& reason) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "mark requires attention"};

    // 先读当前阶段再决定人工恢复后回到哪：retry_wait 沿用已保存的
    // retry_stage，正常执行阶段把它记进 retry_stage，同一事务内落库。
    auto reader = prepare(
        impl_->database,
        "SELECT stage, COALESCE(retry_stage, '') FROM pending_jobs "
        "WHERE execution_key = ?",
        "read attention stage");
    bind_text(impl_->database, reader.get(), 1, execution_key,
              "read attention stage");
    if (sqlite3_step(reader.get()) != SQLITE_ROW) {
        throw AgentQueueError(
            "mark requires attention requires a pending job");
    }
    const auto stage = read_text(reader.get(), 0);
    auto resume_stage = read_text(reader.get(), 1);
    if (stage == "retry_wait") {
        // retry_wait 保存的 retry_stage 必须指向可执行阶段，
        // 缺失或不可执行说明内部状态已经坏了，直接报错不猜。
        if (!is_executable_stage(resume_stage)) {
            throw AgentQueueError(
                "retry_wait job has no executable retry_stage: " +
                execution_key);
        }
    } else if (stage != "requires_attention") {
        resume_stage = stage;
    } else {
        // attention 作业不会再被加载执行，走到这里说明状态流转出了问题。
        throw AgentQueueError(
            "job is already requires_attention: " + execution_key);
    }

    auto statement = prepare(
        impl_->database,
        "UPDATE pending_jobs SET stage = 'requires_attention', "
        "retry_stage = ?, last_error_kind = ?, last_error = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ?",
        "mark requires attention");
    bind_text(impl_->database, statement.get(), 1, resume_stage,
              "mark requires attention");
    bind_text(impl_->database, statement.get(), 2, error_kind,
              "mark requires attention");
    bind_text(impl_->database,
              statement.get(),
              3,
              reason.substr(0, 512),
              "mark requires attention");
    bind_text(impl_->database,
              statement.get(),
              4,
              execution_key,
              "mark requires attention");
    check_result(sqlite3_step(statement.get()), impl_->database,
                 "mark requires attention");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError(
            "mark requires attention requires a pending job");
    }
    transaction.commit();
}

void AgentQueueStore::record_delivery_failure(
    const std::string& request_type,
    const std::string& idempotency_key,
    bool retryable,
    const std::string& error_kind,
    unsigned int http_status,
    const std::string& message,
    std::chrono::milliseconds retry_delay) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "record delivery failure"};
    auto delivery = prepare(
        impl_->database,
        "UPDATE pending_deliveries SET attempt_count = attempt_count + 1, "
        "next_attempt_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', ?), "
        "last_error_kind = ?, last_http_status = ?, last_error = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE request_type = ? AND idempotency_key = ?",
        "record delivery failure");
    const auto modifier = "+" +
        std::to_string(static_cast<double>(retry_delay.count()) / 1000.0) +
        " seconds";
    bind_text(impl_->database, delivery.get(), 1, modifier,
              "record delivery failure");
    bind_text(impl_->database, delivery.get(), 2, error_kind,
              "record delivery failure");
    check_result(sqlite3_bind_int64(delivery.get(), 3, http_status),
                 impl_->database, "record delivery failure");
    bind_text(impl_->database, delivery.get(), 4, message.substr(0, 512),
              "record delivery failure");
    bind_text(impl_->database, delivery.get(), 5, request_type,
              "record delivery failure");
    bind_text(impl_->database, delivery.get(), 6, idempotency_key,
              "record delivery failure");
    check_result(sqlite3_step(delivery.get()), impl_->database,
                 "record delivery failure");
    if (sqlite3_changes(impl_->database) != 1) {
        throw AgentQueueError("delivery does not exist");
    }
    auto job = prepare(
        impl_->database,
        "UPDATE pending_jobs SET retry_stage = stage, stage = ?, "
        "attempt_count = attempt_count + 1, "
        "next_attempt_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', ?), "
        "last_error_kind = ?, last_error = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = (SELECT execution_key FROM pending_deliveries "
        "WHERE request_type = ? AND idempotency_key = ?) "
        "AND stage IN ('start_pending','manifest_pending','report_pending')",
        "record job failure");
    bind_text(impl_->database, job.get(), 1,
              retryable ? "retry_wait" : "requires_attention",
              "record job failure");
    bind_text(impl_->database, job.get(), 2, modifier,
              "record job failure");
    bind_text(impl_->database, job.get(), 3, error_kind,
              "record job failure");
    bind_text(impl_->database, job.get(), 4, message.substr(0, 512),
              "record job failure");
    bind_text(impl_->database, job.get(), 5, request_type,
              "record job failure");
    bind_text(impl_->database, job.get(), 6, idempotency_key,
              "record job failure");
    check_result(sqlite3_step(job.get()), impl_->database,
                 "record job failure");
    auto attempt = prepare(
        impl_->database,
        "INSERT INTO delivery_attempts "
        "(execution_key,request_type,attempt_number,attempted_at,outcome,"
        "error_kind,http_status,message) SELECT execution_key,request_type,"
        "attempt_count,strftime('%Y-%m-%dT%H:%M:%fZ','now'),?,?,?,? "
        "FROM pending_deliveries WHERE request_type = ? AND idempotency_key = ?",
        "insert delivery attempt");
    bind_text(impl_->database, attempt.get(), 1,
              retryable ? "retryable_failure" : "permanent_failure",
              "insert delivery attempt");
    bind_text(impl_->database, attempt.get(), 2, error_kind,
              "insert delivery attempt");
    check_result(sqlite3_bind_int64(attempt.get(), 3, http_status),
                 impl_->database, "insert delivery attempt");
    bind_text(impl_->database, attempt.get(), 4, message.substr(0, 512),
              "insert delivery attempt");
    bind_text(impl_->database, attempt.get(), 5, request_type,
              "insert delivery attempt");
    bind_text(impl_->database, attempt.get(), 6, idempotency_key,
              "insert delivery attempt");
    check_result(sqlite3_step(attempt.get()), impl_->database,
                 "insert delivery attempt");
    transaction.commit();
}

void AgentQueueStore::resume_delivery(const std::string& request_type,
                                      const std::string& idempotency_key) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    // 投递记录必须存在；作业可能已在早前调用（或上次运行中断前）恢复过
    // 阶段，此时 UPDATE 改到 0 行属于幂等成功，不能当作内部错误。
    auto exists = prepare(
        impl_->database,
        "SELECT 1 FROM pending_deliveries "
        "WHERE request_type = ? AND idempotency_key = ?",
        "resume delivery");
    bind_text(impl_->database, exists.get(), 1, request_type,
              "resume delivery");
    bind_text(impl_->database, exists.get(), 2, idempotency_key,
              "resume delivery");
    if (sqlite3_step(exists.get()) != SQLITE_ROW) {
        throw AgentQueueError("delivery does not exist");
    }

    auto statement = prepare(
        impl_->database,
        "UPDATE pending_jobs SET stage = retry_stage, retry_stage = NULL, "
        "next_attempt_at = NULL WHERE execution_key = "
        "(SELECT execution_key FROM pending_deliveries "
        "WHERE request_type = ? AND idempotency_key = ?) "
        "AND stage = 'retry_wait'",
        "resume delivery");
    bind_text(impl_->database, statement.get(), 1, request_type,
              "resume delivery");
    bind_text(impl_->database, statement.get(), 2, idempotency_key,
              "resume delivery");
    check_result(sqlite3_step(statement.get()), impl_->database,
                 "resume delivery");
}

std::chrono::milliseconds AgentQueueStore::delivery_retry_remaining(
    const std::string& request_type,
    const std::string& idempotency_key) const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        "SELECT count(*), COALESCE(max(0, CAST((julianday(next_attempt_at) - "
        "julianday('now')) * 86400000 AS INTEGER)), 0) "
        "FROM pending_deliveries WHERE request_type = ? AND idempotency_key = ?",
        "read delivery retry remaining");
    bind_text(impl_->database, statement.get(), 1, request_type,
              "read delivery retry remaining");
    bind_text(impl_->database, statement.get(), 2, idempotency_key,
              "read delivery retry remaining");
    check_result(sqlite3_step(statement.get()), impl_->database,
                 "read delivery retry remaining");
    if (sqlite3_column_int(statement.get(), 0) != 1) {
        throw AgentQueueError("delivery does not exist");
    }
    return std::chrono::milliseconds{sqlite3_column_int64(statement.get(), 1)};
}

int AgentQueueStore::delivery_attempt_count(
    const std::string& request_type,
    const std::string& idempotency_key) const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        "SELECT attempt_count FROM pending_deliveries "
        "WHERE request_type = ? AND idempotency_key = ?",
        "read delivery attempt count");
    bind_text(impl_->database, statement.get(), 1, request_type,
              "read delivery attempt count");
    bind_text(impl_->database, statement.get(), 2, idempotency_key,
              "read delivery attempt count");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw AgentQueueError("delivery does not exist");
    }
    return sqlite3_column_int(statement.get(), 0);
}

std::string AgentQueueStore::delivery_execution_key(
    const std::string& request_type,
    const std::string& idempotency_key) const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        "SELECT execution_key FROM pending_deliveries "
        "WHERE request_type = ? AND idempotency_key = ?",
        "find delivery job");
    bind_text(impl_->database, statement.get(), 1, request_type,
              "find delivery job");
    bind_text(impl_->database, statement.get(), 2, idempotency_key,
              "find delivery job");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw AgentQueueError("delivery does not exist");
    }
    return read_text(statement.get(), 0);
}

bool AgentQueueStore::has_capacity() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return read_pending_job_count(impl_->database) < impl_->max_pending_jobs;
}

bool AgentQueueStore::is_file_occupied(
    const std::string& task_id,
    const std::string& fingerprint) const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    // 两处都算占用：已经处理完写进 processed_files 的，
    // 以及还挂在任意排队作业（含 requires_attention）计划里的。
    auto statement = prepare(
        impl_->database,
        "SELECT 1 WHERE EXISTS "
        "(SELECT 1 FROM processed_files WHERE task_id = ? AND fingerprint = ?) "
        "OR EXISTS (SELECT 1 FROM pending_files files "
        "JOIN pending_jobs jobs ON jobs.execution_key = files.execution_key "
        "WHERE jobs.task_id = ? AND files.fingerprint = ?)",
        "find occupied fingerprint");
    bind_text(impl_->database, statement.get(), 1, task_id,
              "find occupied fingerprint");
    bind_text(impl_->database, statement.get(), 2, fingerprint,
              "find occupied fingerprint");
    bind_text(impl_->database, statement.get(), 3, task_id,
              "find occupied fingerprint");
    bind_text(impl_->database, statement.get(), 4, fingerprint,
              "find occupied fingerprint");
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

bool AgentQueueStore::is_file_in_flight(
    const std::string& task_id,
    const std::string& fingerprint) const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    // 只看在途：还挂在排队/attention 作业计划里的算占用；
    // 历史 processed 不算——retry 明确选中的历史失败输入允许再次处理。
    auto statement = prepare(
        impl_->database,
        "SELECT 1 WHERE EXISTS (SELECT 1 FROM pending_files files "
        "JOIN pending_jobs jobs ON jobs.execution_key = files.execution_key "
        "WHERE jobs.task_id = ? AND files.fingerprint = ?)",
        "find in-flight fingerprint");
    bind_text(impl_->database, statement.get(), 1, task_id,
              "find in-flight fingerprint");
    bind_text(impl_->database, statement.get(), 2, fingerprint,
              "find in-flight fingerprint");
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

struct AgentQueueMaintenance::Impl {
    sqlite3* database{nullptr};

    ~Impl() {
        if (database != nullptr) {
            sqlite3_close_v2(database);
        }
    }
};

AgentQueueMaintenance::AgentQueueMaintenance(const std::string& database_path,
                                             const std::string& node_code,
                                             bool read_only)
    : impl_(std::make_unique<Impl>()) {
    if (database_path.empty() || node_code.empty()) {
        throw AgentQueueError(
            "database path and node identity are required");
    }
    if (!labbridge::core::fs::exists(labbridge::core::fs::path{
            database_path})) {
        throw AgentQueueError("queue database not found: " + database_path);
    }

    // 维护入口绝不带 CREATE：库必须已经存在，里面缺什么就报什么错。
    const int flags =
        read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    const int open_result = sqlite3_open_v2(
        database_path.c_str(), &impl_->database, flags, nullptr);
    if (open_result != SQLITE_OK) {
        const std::string detail = impl_->database == nullptr
            ? "unknown SQLite open error"
            : sqlite3_errmsg(impl_->database);
        throw AgentQueueError("open queue database failed: " + detail);
    }
    sqlite3_busy_timeout(impl_->database, kBusyTimeoutMilliseconds);

    const int schema_version = read_schema_version(impl_->database);
    if (schema_version == 0) {
        throw AgentQueueError(
            "queue database is empty or not initialized: " + database_path);
    }
    if (schema_version > kSchemaVersion) {
        throw AgentQueueError(
            "unsupported queue schema version " +
            std::to_string(schema_version));
    }
    validate_required_tables(impl_->database);
    validate_node_identity(impl_->database, node_code);
}

AgentQueueMaintenance::~AgentQueueMaintenance() = default;

std::vector<AttentionJobSummary>
AgentQueueMaintenance::list_attention_jobs() const {
    auto statement = prepare(
        impl_->database,
        "SELECT execution_key, task_id, COALESCE(last_error_kind, ''), "
        "COALESCE(last_error, ''), updated_at "
        "FROM pending_jobs WHERE stage = 'requires_attention' "
        "ORDER BY updated_at DESC, execution_key",
        "list attention jobs");

    std::vector<AttentionJobSummary> jobs;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        jobs.push_back({
            read_text(statement.get(), 0),
            read_text(statement.get(), 1),
            read_text(statement.get(), 2),
            read_text(statement.get(), 3),
            read_text(statement.get(), 4),
        });
    }
    return jobs;
}

AttentionJobDetail AgentQueueMaintenance::load_attention_job(
    const std::string& execution_key) const {
    auto statement = prepare(
        impl_->database,
        "SELECT task_id, stage, COALESCE(retry_stage, ''), "
        "COALESCE(last_error_kind, ''), COALESCE(last_error, ''), "
        "attempt_count, started_at, updated_at "
        "FROM pending_jobs WHERE execution_key = ?",
        "load attention job");
    bind_text(impl_->database,
              statement.get(),
              1,
              execution_key,
              "load attention job");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw AgentQueueError("pending job does not exist: " + execution_key);
    }
    const auto stage = read_text(statement.get(), 1);
    if (stage != "requires_attention") {
        throw AgentQueueError(
            "job is not waiting for attention (current stage: " + stage +
            "): " + execution_key);
    }

    AttentionJobDetail detail;
    detail.execution_key = execution_key;
    detail.task_id = read_text(statement.get(), 0);
    detail.resume_stage = read_text(statement.get(), 2);
    detail.error_kind = read_text(statement.get(), 3);
    detail.reason = read_text(statement.get(), 4);
    detail.attempt_count = sqlite3_column_int(statement.get(), 5);
    detail.started_at = read_text(statement.get(), 6);
    detail.updated_at = read_text(statement.get(), 7);
    detail.files = read_attention_files(impl_->database, execution_key);
    detail.deliveries =
        read_attention_deliveries(impl_->database, execution_key);
    return detail;
}

QueueRetryResult AgentQueueMaintenance::retry_attention_job(
    const std::string& execution_key) {
    Transaction transaction{impl_->database, "retry attention job"};

    auto reader = prepare(
        impl_->database,
        "SELECT stage, COALESCE(retry_stage, ''), "
        "COALESCE(last_error_kind, '') FROM pending_jobs "
        "WHERE execution_key = ?",
        "read attention job");
    bind_text(impl_->database,
              reader.get(),
              1,
              execution_key,
              "read attention job");
    if (sqlite3_step(reader.get()) != SQLITE_ROW) {
        throw AgentQueueError("pending job does not exist: " + execution_key);
    }
    const auto stage = read_text(reader.get(), 0);
    if (stage != "requires_attention") {
        // 状态已经不满足（可能刚被恢复过或已完成），明确告诉调用方，
        // 不重复修改；事务没写过任何行，回滚等于空操作。
        return {QueueRetryStatus::NotAttention, stage};
    }

    // 恢复位置：正常路径用转人工时保存的 retry_stage；030-01 之前的
    // archive_conflict 旧数据没存这个字段，归档校验发生在 collecting，
    // 固定回 collecting 重做；其余说不出恢复位置的报错，禁止猜阶段。
    auto resume_stage = read_text(reader.get(), 1);
    const auto error_kind = read_text(reader.get(), 2);
    if (resume_stage.empty()) {
        if (error_kind == "archive_conflict") {
            resume_stage = "collecting";
        } else {
            throw AgentQueueError(
                "cannot determine resume stage for job " + execution_key +
                " (retry_stage empty, error_kind=" + error_kind +
                "); inspect the job manually");
        }
    } else if (!is_executable_stage(resume_stage)) {
        throw AgentQueueError(
            "saved retry_stage '" + resume_stage + "' of job " +
            execution_key + " is not an executable stage");
    }

    auto job = prepare(
        impl_->database,
        "UPDATE pending_jobs SET stage = ?, retry_stage = NULL, "
        "next_attempt_at = NULL, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ? AND stage = 'requires_attention'",
        "resume attention job");
    bind_text(impl_->database,
              job.get(),
              1,
              resume_stage,
              "resume attention job");
    bind_text(impl_->database,
              job.get(),
              2,
              execution_key,
              "resume attention job");
    check_result(
        sqlite3_step(job.get()), impl_->database, "resume attention job");
    if (sqlite3_changes(impl_->database) != 1) {
        return {QueueRetryStatus::NotAttention, stage};
    }

    // 对应投递的退避时间一并清掉：Agent 启动后立刻接着投，不再等旧退避。
    // 执行键、请求、任务快照、归档、指纹和失败记录都不动，尝试次数不清零。
    auto deliveries = prepare(
        impl_->database,
        "UPDATE pending_deliveries SET next_attempt_at = NULL, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE execution_key = ?",
        "resume attention deliveries");
    bind_text(impl_->database,
              deliveries.get(),
              1,
              execution_key,
              "resume attention deliveries");
    check_result(sqlite3_step(deliveries.get()),
                 impl_->database,
                 "resume attention deliveries");
    transaction.commit();
    return {QueueRetryStatus::Resumed, resume_stage};
}

AgentQueueLock::AgentQueueLock(const std::string& database_path) {
    if (database_path.empty()) {
        throw AgentQueueError("database path is required");
    }
    const std::string lock_path = database_path + ".lock";
    file_descriptor_ =
        ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (file_descriptor_ < 0) {
        throw AgentQueueError("cannot open queue lock file " + lock_path +
                              ": " + std::strerror(errno));
    }
    // 非阻塞抢锁：抢不到说明有个 Agent（或另一个 retry）正拿着。
    // 锁随进程退出由内核释放，这里等下去没有意义，直接报错让运维先停服务。
    if (::flock(file_descriptor_, LOCK_EX | LOCK_NB) != 0) {
        const int reason = errno;
        ::close(file_descriptor_);
        file_descriptor_ = -1;
        throw AgentQueueError(
            "queue is locked by a running process; stop the agent first (" +
            lock_path + ": " + std::strerror(reason) + ")");
    }
}

AgentQueueLock::~AgentQueueLock() {
    if (file_descriptor_ >= 0) {
        // 关闭描述符即释放 flock，内核保证，不需要显式 unlock。
        ::close(file_descriptor_);
    }
}

}  // namespace labbridge::agent
