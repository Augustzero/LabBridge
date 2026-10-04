#include "support/server/in_memory_repositories.h"

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

// 测试替身没有行锁语义，锁读退化为普通读取。
std::optional<AlertRecord> InMemoryAlertRepository::lock_by_id(
    const std::string& alert_id) {
    const auto found = alerts_.find(alert_id);
    if (found == alerts_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<AlertRecord> InMemoryAlertRepository::save_acknowledged(
    const std::string& alert_id) {
    const auto found = alerts_.find(alert_id);
    if (found == alerts_.end()) {
        return std::nullopt;
    }
    found->second.status = "acknowledged";
    if (found->second.acknowledged_at.empty()) {
        found->second.acknowledged_at = "in-memory-acknowledged-at";
    }
    return found->second;
}

std::optional<AlertRecord> InMemoryAlertRepository::save_closed(
    const std::string& alert_id) {
    const auto found = alerts_.find(alert_id);
    if (found == alerts_.end()) {
        return std::nullopt;
    }
    found->second.status = "closed";
    if (found->second.closed_at.empty()) {
        found->second.closed_at = "in-memory-closed-at";
    }
    return found->second;
}

}  // namespace labbridge::server
