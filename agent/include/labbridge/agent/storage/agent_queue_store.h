#pragma once

#include "labbridge/agent/execution/task_execution_client.h"
#include "labbridge/core/models.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace labbridge::agent {

class AgentQueueError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct PendingFilePlan {
    int ordinal{0};
    std::string source_path;
    std::string original_name;
    std::string source_mtime;
    long long size_bytes{0};
    std::string file_hash;
    std::string fingerprint;
    std::string archive_path;
};

struct RecoveredJob {
    std::string execution_key;
    labbridge::core::TaskConfig task;
    StartTaskRunRequest start_request;
    std::string stage;
    std::vector<PendingFilePlan> files;
};

class AgentQueueStore {
public:
    AgentQueueStore(std::string database_path,
                    std::string node_code,
                    std::size_t max_pending_jobs);
    ~AgentQueueStore();
    AgentQueueStore(const AgentQueueStore&) = delete;
    AgentQueueStore& operator=(const AgentQueueStore&) = delete;

    bool begin_job(const labbridge::core::TaskConfig& task,
                   const StartTaskRunRequest& request);
    void save_file_plan(const std::string& execution_key,
                        const std::vector<PendingFilePlan>& files);
    std::vector<RecoveredJob> recover_jobs() const;
    std::size_t pending_job_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace labbridge::agent
