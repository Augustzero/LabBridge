#include "labbridge/agent/storage/agent_queue_store.h"

#include "labbridge/agent/execution/execution_request_codec.h"
#include "labbridge/core/filesystem.h"

#include <sqlite3.h>

#include <array>
#include <memory>
#include <mutex>
#include <utility>

namespace labbridge::agent {
namespace {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

constexpr int kSchemaVersion = 1;
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

void check_result(int result,
                  sqlite3* database,
                  const std::string& operation) {
    if (result == SQLITE_OK || result == SQLITE_DONE || result == SQLITE_ROW) {
        return;
    }

    throw AgentQueueError(
        operation + " failed: sqlite code=" + std::to_string(result) + " " +
        sqlite3_errmsg(database));
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
        raw_message == nullptr ? sqlite3_errmsg(database) : raw_message;
    sqlite3_free(raw_message);
    throw AgentQueueError(
        operation + " failed: sqlite code=" + std::to_string(result) + " " +
        detail);
}

Statement prepare(sqlite3* database,
                  const char* sql,
                  const std::string& operation) {
    sqlite3_stmt* raw_statement = nullptr;
    check_result(
        sqlite3_prepare_v2(database, sql, -1, &raw_statement, nullptr),
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

    execute(database, "PRAGMA user_version=1", "set schema version");
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

void insert_pending_job(sqlite3* database,
                        const StartTaskRunRequest& request,
                        const std::string& task_json) {
    auto statement = prepare(
        database,
        "INSERT INTO pending_jobs "
        "(execution_key, node_code, task_id, scheduled_for, started_at, "
        "task_config_json, stage, created_at, updated_at) VALUES "
        "(?, ?, ?, ?, ?, ?, 'start_pending', "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        "insert pending job");

    const std::array<const std::string*, 6> values{
        &request.execution_key,
        &request.node_code,
        &request.task_id,
        &request.scheduled_for,
        &request.started_at,
        &task_json,
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
        "size_bytes, file_hash, fingerprint, archive_path) VALUES "
        "(?, ?, ?, ?, ?, ?, ?, ?, ?)",
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
        "COALESCE(raw_file_id, ''), COALESCE(parsed_without_errors, 0) "
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
        });
    }
    return files;
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

bool AgentQueueStore::begin_job(const labbridge::core::TaskConfig& task,
                                const StartTaskRunRequest& request) {
    if (request.node_code != impl_->node_code ||
        task.node_code != impl_->node_code || request.task_id != task.id) {
        throw AgentQueueError("job identity does not match queue identity");
    }

    const auto task_json = encode_task_config(task);
    const auto request_json = encode_start_task_run_request(request);

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
    insert_pending_job(impl_->database, request, task_json);
    insert_start_delivery(impl_->database, request, request_json);
    transaction.commit();
    return true;
}

void AgentQueueStore::save_file_plan(
    const std::string& execution_key,
    const std::vector<PendingFilePlan>& files) {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Transaction transaction{impl_->database, "file plan"};

    // 完整文件计划按批次提交，任何一条约束失败都不能留下部分计划。
    for (const auto& file : files) {
        insert_pending_file(impl_->database, execution_key, file);
    }
    transaction.commit();
}

std::vector<RecoveredJob> AgentQueueStore::recover_jobs() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        "SELECT jobs.execution_key, jobs.task_config_json, CASE WHEN jobs.stage = 'retry_wait' THEN jobs.retry_stage ELSE jobs.stage END, "
        "deliveries.request_json, COALESCE(jobs.task_run_id, ''), "
        "COALESCE((SELECT request_json FROM pending_deliveries "
        "WHERE execution_key = jobs.execution_key AND request_type = 'manifest'), ''), "
        "COALESCE((SELECT request_json FROM pending_deliveries "
        "WHERE execution_key = jobs.execution_key AND request_type = 'report'), '') "
        "FROM pending_jobs AS jobs "
        "JOIN pending_deliveries AS deliveries "
        "ON deliveries.execution_key = jobs.execution_key "
        "AND deliveries.request_type = 'start' WHERE jobs.stage != 'requires_attention' "
        "ORDER BY jobs.created_at, jobs.execution_key",
        "recover jobs");

    std::vector<RecoveredJob> jobs;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        RecoveredJob job;
        job.execution_key = read_text(statement.get(), 0);
        job.task = decode_task_config(read_text(statement.get(), 1));
        job.stage = read_text(statement.get(), 2);
        job.start_request =
            decode_start_task_run_request(read_text(statement.get(), 3));
        job.task_run_id = read_text(statement.get(), 4);
        const auto manifest_json = read_text(statement.get(), 5);
        if (!manifest_json.empty()) {
            job.manifest_request =
                decode_raw_file_manifest_request(manifest_json);
        }
        const auto report_json = read_text(statement.get(), 6);
        if (!report_json.empty()) {
            job.report_request =
                decode_task_run_report_request(report_json);
        }
        job.files = recover_file_plan(impl_->database, job.execution_key);
        jobs.push_back(std::move(job));
    }
    return jobs;
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
    auto count = prepare(
        impl_->database,
        "SELECT count(*) FROM pending_files WHERE execution_key = ?",
        "count manifest files");
    bind_text(impl_->database, count.get(), 1, execution_key,
              "count manifest files");
    check_result(sqlite3_step(count.get()), impl_->database,
                 "count manifest files");
    if (sqlite3_column_int64(count.get(), 0) !=
        static_cast<sqlite3_int64>(raw_file_ids.size())) {
        throw AgentQueueError("manifest raw ID count mismatch");
    }
    auto update = prepare(
        impl_->database,
        "UPDATE pending_files SET raw_file_id = ? "
        "WHERE execution_key = ? AND ordinal = ?",
        "map raw file ID");
    for (std::size_t index = 0; index < raw_file_ids.size(); ++index) {
        sqlite3_reset(update.get());
        sqlite3_clear_bindings(update.get());
        bind_text(impl_->database, update.get(), 1, raw_file_ids[index],
                  "map raw file ID");
        bind_text(impl_->database, update.get(), 2, execution_key,
                  "map raw file ID");
        check_result(sqlite3_bind_int(update.get(), 3, static_cast<int>(index)),
                     impl_->database, "map raw file ID");
        check_result(sqlite3_step(update.get()), impl_->database,
                     "map raw file ID");
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
        "SELECT max(0, CAST((julianday(next_attempt_at) - "
        "julianday('now')) * 86400000 AS INTEGER)) "
        "FROM pending_deliveries WHERE request_type = ? AND idempotency_key = ?",
        "read delivery retry remaining");
    bind_text(impl_->database, statement.get(), 1, request_type,
              "read delivery retry remaining");
    bind_text(impl_->database, statement.get(), 2, idempotency_key,
              "read delivery retry remaining");
    check_result(sqlite3_step(statement.get()), impl_->database,
                 "read delivery retry remaining");
    return std::chrono::milliseconds{sqlite3_column_int64(statement.get(), 0)};
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
    check_result(sqlite3_step(statement.get()), impl_->database,
                 "read delivery attempt count");
    return sqlite3_column_int(statement.get(), 0);
}

bool AgentQueueStore::has_capacity() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return read_pending_job_count(impl_->database) < impl_->max_pending_jobs;
}

bool AgentQueueStore::is_file_processed(
    const std::string& task_id,
    const std::string& fingerprint) const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    auto statement = prepare(
        impl_->database,
        "SELECT 1 FROM processed_files WHERE task_id = ? AND fingerprint = ?",
        "find processed fingerprint");
    bind_text(impl_->database, statement.get(), 1, task_id,
              "find processed fingerprint");
    bind_text(impl_->database, statement.get(), 2, fingerprint,
              "find processed fingerprint");
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

}  // namespace labbridge::agent
