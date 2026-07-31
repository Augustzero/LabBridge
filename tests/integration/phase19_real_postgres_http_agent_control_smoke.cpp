#include "labbridge/core/version.h"
#include "labbridge/server/agent_control_http_controller.h"
#include "labbridge/server/config_service.h"
#include "labbridge/server/libpq_sql_session.h"
#include "labbridge/server/postgres_agent_control_executor.h"
#include "labbridge/server/postgres_config_repository.h"
#include "labbridge/server/postgres_node_repository.h"
#include "labbridge/server/storage_mapping.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/writer.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

drogon::HttpResponsePtr invoke_post(
    const labbridge::server::AgentControlHttpController& controller,
    const Json::Value& body,
    bool heartbeat) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(write_json(body));

    drogon::HttpResponsePtr response;
    auto callback = [&response](const drogon::HttpResponsePtr& current) {
        response = current;
    };
    if (heartbeat) {
        controller.post_heartbeat(request, std::move(callback));
    } else {
        controller.post_register(request, std::move(callback));
    }
    assert(response != nullptr);
    return response;
}

drogon::HttpResponsePtr invoke_config(
    const labbridge::server::AgentControlHttpController& controller,
    const std::string& node_code) {
    drogon::HttpResponsePtr response;
    controller.get_config(
        node_code,
        [&response](const drogon::HttpResponsePtr& current) {
            response = current;
        });
    assert(response != nullptr);
    return response;
}

const Json::Value& response_json(const drogon::HttpResponsePtr& response) {
    const auto& json = response->getJsonObject();
    assert(json != nullptr);
    return *json;
}

bool contains_task(const Json::Value& tasks, const std::string& task_id) {
    for (const auto& task : tasks) {
        if (task["id"].asString() == task_id) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    const char* connection_info = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection_info == nullptr || std::string(connection_info).empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping real PostgreSQL HTTP agent control smoke test\n";
        return 77;
    }

    auto executor =
        std::make_shared<labbridge::server::PostgresAgentControlExecutor>(
            connection_info);
    labbridge::server::AgentControlHttpController controller{
        [executor](const labbridge::core::NodeInfo& node) {
            return executor->register_node(node);
        },
        [executor](const labbridge::core::NodeHeartbeat& heartbeat) {
            return executor->accept_heartbeat(heartbeat);
        },
        [executor](const std::string& node_code) {
            return executor->find_config(node_code);
        }};

    const std::string node_code = "lab-node-real-agent-control-019";
    Json::Value registration;
    registration["node_code"] = node_code;
    registration["name"] = "phase19 real HTTP agent node";
    registration["agent_version"] = labbridge::core::kVersion;

    const auto register_response = invoke_post(controller, registration, false);
    assert(register_response->statusCode() == drogon::k201Created);
    assert(response_json(register_response)["ok"].asBool());

    labbridge::server::LibpqSqlSession session{connection_info};
    labbridge::server::PostgresNodeRepository node_repository{session};
    labbridge::server::PostgresConfigRepository config_repository{session};
    labbridge::server::ConfigService config_service{
        node_repository,
        config_repository};

    const auto data_source = config_service.create_data_source({
        node_code,
        labbridge::core::SourceType::LocalDirectory,
        "phase19 real local directory",
        R"({"path":"/data/incoming","pattern":"*.csv"})",
        true,
    });
    assert(data_source.status.ok);

    const auto enabled_task = config_service.create_task({
        node_code,
        data_source.id,
        "phase19 real enabled task",
        "collect_parse_qc",
        "*/5 * * * *",
        "csv_observation",
        "basic",
        true,
    });
    assert(enabled_task.status.ok);

    const auto disabled_task = config_service.create_task({
        node_code,
        data_source.id,
        "phase19 real disabled task",
        "collect_parse_qc",
        "manual",
        "csv_observation",
        "basic",
        false,
    });
    assert(disabled_task.status.ok);

    Json::Value heartbeat;
    heartbeat["node_code"] = node_code;
    heartbeat["agent_version"] = labbridge::core::kVersion;
    heartbeat["reported_at"] = "2026-07-17 10:30:00+08";
    const auto heartbeat_response = invoke_post(controller, heartbeat, true);
    assert(heartbeat_response->statusCode() == drogon::k200OK);
    assert(response_json(heartbeat_response)["data"]["status"].asString() ==
           "online");

    const auto config_response = invoke_config(controller, node_code);
    assert(config_response->statusCode() == drogon::k200OK);
    const auto& config = response_json(config_response)["data"];
    assert(config["node"]["node_code"].asString() == node_code);
    assert(config["node"]["status"].asString() == "online");
    assert(!config["node"]["last_heartbeat_at"].asString().empty());
    assert(contains_task(config["tasks"], enabled_task.id));
    assert(!contains_task(config["tasks"], disabled_task.id));

    const auto missing_response =
        invoke_config(controller, "lab-node-real-agent-control-019-missing");
    assert(missing_response->statusCode() == drogon::k404NotFound);
    assert(response_json(missing_response)["error"]["code"].asString() ==
           "not_found");

    const auto persisted = session.query_one(
        "SELECT n.status, n.agent_version, "
        "COALESCE(to_char(n.last_heartbeat_at, 'YYYY-MM-DD HH24:MI:SS'), '') "
        "AS last_heartbeat_at, "
        "(SELECT count(*)::text FROM tasks t "
        " WHERE t.node_id = n.id AND t.enabled = true) AS enabled_task_count "
        "FROM nodes n WHERE n.node_code = $1 LIMIT 1",
        {node_code});
    assert(persisted.has_value());
    assert(labbridge::server::storage::value_or_empty(*persisted, "status") ==
           "online");
    assert(labbridge::server::storage::value_or_empty(
               *persisted, "agent_version") == labbridge::core::kVersion);
    assert(!labbridge::server::storage::value_or_empty(
                *persisted, "last_heartbeat_at")
                .empty());
    assert(std::stoll(labbridge::server::storage::value_or_empty(
               *persisted, "enabled_task_count")) >= 1);

    return 0;
}
