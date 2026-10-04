#pragma once

#include <optional>
#include <string>
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
    std::string created_at;
    // 处置时间沿用空字符串表示 NULL 的存储约定；直接关闭的告警确认时间为空。
    std::string acknowledged_at;
    std::string closed_at;
};

class IAlertRepository {
public:
    virtual ~IAlertRepository() = default;

    virtual std::string create(AlertRecord alert) = 0;
    virtual std::vector<AlertRecord> find_by_node(const std::string& node_code) const = 0;
    virtual std::vector<AlertRecord> find_by_task_run(const std::string& task_run_id) const = 0;

    // 按主键加行锁读取：确认/关闭的并发处置靠这把锁串行化，
    // 调用方必须在同一事务内先锁再决定是否写入。
    virtual std::optional<AlertRecord> lock_by_id(const std::string& alert_id) = 0;
    // 两个写入方法只改状态并补首次转换时间，不做状态机判断；
    // 重复确认/关闭不会刷新已有时间（COALESCE 语义）。
    virtual std::optional<AlertRecord> save_acknowledged(const std::string& alert_id) = 0;
    virtual std::optional<AlertRecord> save_closed(const std::string& alert_id) = 0;
};

}  // namespace labbridge::server
