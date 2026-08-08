#include "labbridge/core/logging.h"
#include "labbridge/core/version.h"
#include "labbridge/server/http/agent_control_http_controller.h"
#include "labbridge/server/http/agent_report_http_controller.h"
#include "labbridge/server/http/task_run_http_controller.h"
#include "labbridge/server/postgres/agent_control_executor.h"
#include "labbridge/server/postgres/agent_report_executor.h"
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
