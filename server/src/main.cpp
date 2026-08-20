#include "labbridge/core/logging.h"
#include "labbridge/core/version.h"
#include "labbridge/server/http/agent_control_http_controller.h"
#include "labbridge/server/http/agent_report_http_controller.h"
#include "labbridge/server/http/management_http_controller.h"
#include "labbridge/server/http/task_run_http_controller.h"
#include "labbridge/server/postgres/agent_control_executor.h"
#include "labbridge/server/postgres/agent_report_executor.h"
#include "labbridge/server/postgres/management_query_executor.h"
#include "labbridge/server/postgres/task_run_executor.h"

#include <drogon/drogon.h>

#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kComponent = "server";

std::string database_connection_info(const drogon::HttpAppFramework& app) {
    const char* environment_value = std::getenv("LABBRIDGE_DATABASE_URL");
    if (environment_value != nullptr && environment_value[0] != '\0') {
        return environment_value;
    }

    const auto& custom_config = app.getCustomConfig();
    if (!custom_config.isObject() || !custom_config.isMember("database") ||
        !custom_config["database"].isObject() ||
        !custom_config["database"].isMember("connection_info") ||
        !custom_config["database"]["connection_info"].isString()) {
        throw std::runtime_error(
            "database connection_info is missing from server configuration");
    }

    const auto connection_info =
        custom_config["database"]["connection_info"].asString();
    if (connection_info.empty()) {
        throw std::runtime_error("database connection_info must not be empty");
    }
    return connection_info;
}

int positive_management_setting(const drogon::HttpAppFramework& app,
                                const std::string& name) {
    const auto& custom_config = app.getCustomConfig();
    if (!custom_config.isObject() ||
        !custom_config.isMember("management") ||
        !custom_config["management"].isObject() ||
        !custom_config["management"].isMember(name) ||
        !custom_config["management"][name].isInt()) {
        throw std::runtime_error(
            "management " + name + " is missing from server configuration");
    }
    const int value = custom_config["management"][name].asInt();
    if (value <= 0) {
        throw std::runtime_error("management " + name + " must be positive");
    }
    return value;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string config_path = argc > 1 ? argv[1] : "deploy/env/server.example.yaml";

    try {
        labbridge::core::log_info(kComponent, "starting LabBridge control plane");
        labbridge::core::log_info(kComponent, labbridge::core::kVersion);
        labbridge::core::log_info(kComponent, "config path: " + config_path);

        auto& app = drogon::app();
        app.loadConfigFile(config_path);

        const auto connection_info = database_connection_info(app);
        const int node_offline_after_seconds = positive_management_setting(
            app, "node_offline_after_seconds");
        const int task_run_stale_after_seconds = positive_management_setting(
            app, "task_run_stale_after_seconds");

        auto report_executor =
            std::make_shared<labbridge::server::PostgresAgentReportExecutor>(
                connection_info);
        auto controller =
            std::make_shared<labbridge::server::AgentReportHttpController>(
                [report_executor](const labbridge::server::RawFileManifestRequest& request) {
                    return report_executor->accept_raw_file_manifest(request);
                },
                [report_executor](const labbridge::server::TaskRunReportRequest& request) {
                    return report_executor->accept_task_run_report(request);
                });
        controller->register_routes(app);

        auto task_run_executor =
            std::make_shared<labbridge::server::PostgresTaskRunExecutor>(
                connection_info);
        auto task_run_controller =
            std::make_shared<labbridge::server::TaskRunHttpController>(
                [task_run_executor](const labbridge::server::StartTaskRunRequest& request) {
                    return task_run_executor->start(request);
                });
        task_run_controller->register_routes(app);

        auto control_executor =
            std::make_shared<labbridge::server::PostgresAgentControlExecutor>(
                connection_info);
        auto control_controller =
            std::make_shared<labbridge::server::AgentControlHttpController>(
                [control_executor](const labbridge::core::NodeInfo& node) {
                    return control_executor->register_node(node);
                },
                [control_executor](const labbridge::core::NodeHeartbeat& heartbeat) {
                    return control_executor->accept_heartbeat(heartbeat);
                },
                [control_executor](const std::string& node_code) {
                    return control_executor->find_config(node_code);
                });
        control_controller->register_routes(app);

        auto management_executor = std::make_shared<
            labbridge::server::PostgresManagementQueryExecutor>(
                connection_info,
                node_offline_after_seconds,
                task_run_stale_after_seconds);
        labbridge::server::ManagementQueryHandlers management_handlers;
        management_handlers.list_nodes =
            [management_executor](
                const labbridge::server::NodeListRequest& request) {
                return management_executor->list_nodes(request);
            };
        management_handlers.find_node =
            [management_executor](const std::string& node_code) {
                return management_executor->find_node(node_code);
            };
        management_handlers.list_data_sources =
            [management_executor](
                const labbridge::server::NodeScopedListRequest& request) {
                return management_executor->list_data_sources(request);
            };
        management_handlers.list_qc_rules =
            [management_executor](
                const labbridge::server::QcRuleListRequest& request) {
                return management_executor->list_qc_rules(request);
            };
        management_handlers.list_tasks =
            [management_executor](
                const labbridge::server::NodeScopedListRequest& request) {
                return management_executor->list_tasks(request);
            };
        management_handlers.list_task_runs =
            [management_executor](
                const labbridge::server::TaskRunListRequest& request) {
                return management_executor->list_task_runs(request);
            };
        management_handlers.find_task_run =
            [management_executor](const std::string& node_code,
                                  const std::string& task_run_id) {
                return management_executor->find_task_run(
                    node_code, task_run_id);
            };
        management_handlers.list_raw_files =
            [management_executor](
                const labbridge::server::RunScopedListRequest& request) {
                return management_executor->list_raw_files(request);
            };
        management_handlers.list_parsed_records =
            [management_executor](
                const labbridge::server::RunScopedListRequest& request) {
                return management_executor->list_parsed_records(request);
            };
        management_handlers.list_qc_results =
            [management_executor](
                const labbridge::server::QcResultListRequest& request) {
                return management_executor->list_qc_results(request);
            };
        management_handlers.list_alerts =
            [management_executor](
                const labbridge::server::AlertListRequest& request) {
                return management_executor->list_alerts(request);
            };
        auto management_controller =
            std::make_shared<labbridge::server::ManagementHttpController>(
                std::move(management_handlers));
        management_controller->register_routes(app);

        labbridge::core::log_info(kComponent, "control plane HTTP service is ready");
        app.run();
        return 0;
    } catch (const std::exception& error) {
        labbridge::core::log_error(kComponent, error.what());
        return 1;
    } catch (...) {
        labbridge::core::log_error(kComponent, "unknown startup failure");
        return 1;
    }
}
