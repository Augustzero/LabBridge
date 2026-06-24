#include "labbridge/server/alert_repository.h"

#include <utility>

namespace labbridge::server {

std::string InMemoryAlertRepository::create(AlertRecord alert) {
    if (alert.id.empty()) {
        alert.id = std::to_string(next_alert_id_++);
    }
    if (alert.status.empty()) {
        alert.status = "open";
    }

    const auto id = alert.id;
    alerts_[id] = std::move(alert);
    return id;
}

std::optional<AlertRecord> InMemoryAlertRepository::find_by_id(
    const std::string& alert_id) const {
    const auto iter = alerts_.find(alert_id);
    if (iter == alerts_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

std::vector<AlertRecord> InMemoryAlertRepository::find_by_node(
    const std::string& node_code) const {
    std::vector<AlertRecord> alerts;
    for (const auto& [id, alert] : alerts_) {
        if (alert.node_code == node_code) {
            alerts.push_back(alert);
        }
    }
    return alerts;
}

std::vector<AlertRecord> InMemoryAlertRepository::find_by_task_run(
    const std::string& task_run_id) const {
    std::vector<AlertRecord> alerts;
    for (const auto& [id, alert] : alerts_) {
        if (alert.task_run_id == task_run_id) {
            alerts.push_back(alert);
        }
    }
    return alerts;
}

}  // namespace labbridge::server
