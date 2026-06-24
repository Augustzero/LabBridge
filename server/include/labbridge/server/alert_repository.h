#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace labbridge::server {

struct AlertRecord {
    std::string id;
    std::string node_code;
    std::string task_run_id;
    std::string alert_type;
    std::string severity;
    std::string message;
    std::string status{"open"};
};

class IAlertRepository {
public:
    virtual ~IAlertRepository() = default;

    virtual std::string create(AlertRecord alert) = 0;
    virtual std::optional<AlertRecord> find_by_id(const std::string& alert_id) const = 0;
    virtual std::vector<AlertRecord> find_by_node(const std::string& node_code) const = 0;
    virtual std::vector<AlertRecord> find_by_task_run(const std::string& task_run_id) const = 0;
};

class InMemoryAlertRepository final : public IAlertRepository {
public:
    std::string create(AlertRecord alert) override;
    std::optional<AlertRecord> find_by_id(const std::string& alert_id) const override;
    std::vector<AlertRecord> find_by_node(const std::string& node_code) const override;
    std::vector<AlertRecord> find_by_task_run(const std::string& task_run_id) const override;

private:
    int next_alert_id_{1};
    std::unordered_map<std::string, AlertRecord> alerts_;
};

}  // namespace labbridge::server
