#pragma once

#include "labbridge/agent/execution/reliable_execution_store.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace labbridge::agent {

class AgentQueueError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AgentQueueStore final : public IReliableExecutionStore {
public:
    AgentQueueStore(std::string database_path,
                    std::string node_code,
                    std::size_t max_pending_jobs,
                    std::size_t processed_fingerprint_capacity = 10000);
    ~AgentQueueStore();
    AgentQueueStore(const AgentQueueStore&) = delete;
    AgentQueueStore& operator=(const AgentQueueStore&) = delete;

    bool begin_job(const labbridge::core::TaskConfig& task,
                   const StartTaskRunRequest& request) override;
    void save_file_plan(const std::string& execution_key,
                        const std::vector<PendingFilePlan>& files) override;
    std::vector<RecoveredJob> recover_jobs() const override;
    void accept_start(const std::string& execution_key,
                      const std::string& task_run_id) override;
    void mark_file_archived(const std::string& execution_key,
                            int ordinal) override;
    void save_manifest(const std::string& execution_key,
                       const RawFileManifestRequest& request) override;
    void accept_manifest(const std::string& execution_key,
                         const std::vector<std::string>& raw_file_ids) override;
    void save_report(const std::string& execution_key,
                     const TaskRunReportRequest& request,
                     const std::vector<bool>& parsed_without_errors) override;
    void complete_job(const std::string& execution_key) override;
    bool is_file_processed(const std::string& task_id,
                           const std::string& fingerprint) const override;
    std::size_t pending_job_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace labbridge::agent
