#pragma once

#include "labbridge/server/repositories/agent_report_receipt_repository.h"
#include "labbridge/server/repositories/alert_repository.h"
#include "labbridge/server/repositories/config_repository.h"
#include "labbridge/server/repositories/node_repository.h"
#include "labbridge/server/repositories/qc_repository.h"
#include "labbridge/server/repositories/result_repository.h"
#include "labbridge/server/repositories/task_run_repository.h"

#include <string>
#include <unordered_map>

namespace labbridge::server {

class InMemoryNodeRepository final : public INodeRepository {
public:
    void upsert(NodeRecord node) override;
    std::optional<NodeRecord> find_by_code(
        const std::string& node_code) const override;

private:
    std::unordered_map<std::string, NodeRecord> nodes_;
};

class InMemoryConfigRepository final : public IConfigRepository {
public:
    std::string create_data_source(DataSourceRecord data_source) override;
    std::optional<DataSourceRecord> find_data_source(
        const std::string& data_source_id) const override;
    std::string create_task(TaskRecord task) override;
    std::optional<TaskRecord> find_task(
        const std::string& task_id) const override;
    std::vector<TaskRecord> find_enabled_tasks_by_node(
        const std::string& node_code) const override;
    std::vector<DataSourceRecord> find_enabled_data_sources_by_node(
        const std::string& node_code) const override;
    std::vector<TaskQcRuleBinding> find_enabled_task_qc_rules_by_node(
        const std::string& node_code) const override;
    void bind_task_qc_rule(const std::string& task_id,
                           const std::string& qc_rule_id,
                           int sort_order) override;
    void add_task_qc_rule_projection(TaskQcRuleBinding binding);


private:
    int next_data_source_id_{1};
    int next_task_id_{1};
    std::unordered_map<std::string, DataSourceRecord> data_sources_;
    std::unordered_map<std::string, TaskRecord> tasks_;
    std::vector<TaskQcRuleBinding> task_qc_rules_;
};

class InMemoryTaskRunRepository final : public ITaskRunRepository {
public:
    std::string create(TaskRunRecord task_run) override;
    std::optional<TaskRunRecord> find_by_id(
        const std::string& task_run_id) const override;
    void finish(TaskRunRecord task_run) override;

private:
    int next_task_run_id_{1};
    std::unordered_map<std::string, TaskRunRecord> task_runs_;
};

class InMemoryResultRepository final : public IResultRepository {
public:
    std::string create_raw_file(RawFileRecord raw_file) override;
    std::optional<RawFileRecord> find_raw_file(
        const std::string& raw_file_id) const override;
    std::vector<RawFileRecord> find_raw_files_by_run(
        const std::string& task_run_id) const override;
    std::string create_parsed_record(ParsedRecordRecord parsed_record) override;
    std::optional<ParsedRecordRecord> find_parsed_record(
        const std::string& parsed_record_id) const override;
    std::vector<ParsedRecordRecord> find_parsed_records_by_run(
        const std::string& task_run_id) const override;

private:
    int next_raw_file_id_{1};
    int next_parsed_record_id_{1};
    std::unordered_map<std::string, RawFileRecord> raw_files_;
    std::unordered_map<std::string, ParsedRecordRecord> parsed_records_;
};

class InMemoryQcRepository final : public IQcRepository {
public:
    std::string create_rule(QcRuleRecord rule) override;
    std::optional<QcRuleRecord> find_rule(
        const std::string& qc_rule_id) const override;
    std::string create_result(QcResultRecord result) override;
    std::optional<QcResultRecord> find_result(
        const std::string& qc_result_id) const override;
    std::vector<QcResultRecord> find_results_by_parsed_record(
        const std::string& parsed_record_id) const override;

private:
    int next_qc_rule_id_{1};
    int next_qc_result_id_{1};
    std::unordered_map<std::string, QcRuleRecord> rules_;
    std::unordered_map<std::string, QcResultRecord> results_;
};

class InMemoryAlertRepository final : public IAlertRepository {
public:
    std::string create(AlertRecord alert) override;
    std::optional<AlertRecord> find_by_id(
        const std::string& alert_id) const override;
    std::vector<AlertRecord> find_by_node(
        const std::string& node_code) const override;
    std::vector<AlertRecord> find_by_task_run(
        const std::string& task_run_id) const override;

private:
    int next_alert_id_{1};
    std::unordered_map<std::string, AlertRecord> alerts_;
};

class InMemoryAgentReportReceiptRepository final
    : public IAgentReportReceiptRepository {
public:
    AgentReportReceiptClaimResult claim(
        AgentReportReceiptClaim request) override;
    void complete(const std::string& receipt_id,
                  AgentReportReceiptResponse response) override;

private:
    int next_receipt_id_{1};
    std::unordered_map<std::string, AgentReportReceipt> receipts_;
};

}  // namespace labbridge::server
