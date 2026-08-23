#include "labbridge/server/http/management_http_controller.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/management_command_executor.h"
#include "labbridge/server/postgres/management_query_executor.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <json/writer.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace labbridge::server;
using labbridge::server::storage::value_or_empty;

drogon::HttpRequestPtr request(
    std::initializer_list<std::pair<std::string, std::string>> parameters = {}) {
    auto value = drogon::HttpRequest::newHttpRequest();
    value->setMethod(drogon::Get);
    for (const auto& parameter : parameters) {
        value->setParameter(parameter.first, parameter.second);
    }
    return value;
}

drogon::HttpRequestPtr json_request(drogon::HttpMethod method,
                                    const Json::Value& body) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    auto value = drogon::HttpRequest::newHttpRequest();
    value->setMethod(method);
    value->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    value->setBody(Json::writeString(builder, body));
    return value;
}

template <typename Invoke>
drogon::HttpResponsePtr invoke(Invoke operation) {
    drogon::HttpResponsePtr response;
    operation([&response](const drogon::HttpResponsePtr& current) {
        response = current;
    });
    EXPECT_NE(response, nullptr);
    return response;
}

const Json::Value& data(const drogon::HttpResponsePtr& response) {
    EXPECT_EQ(response->statusCode(), drogon::k200OK);
    const auto& json = response->getJsonObject();
    EXPECT_NE(json, nullptr);
    EXPECT_TRUE((*json)["ok"].asBool());
    return (*json)["data"];
}

const Json::Value& success_data(
    const drogon::HttpResponsePtr& response,
    drogon::HttpStatusCode expected_status) {
    EXPECT_EQ(response->statusCode(), expected_status);
    const auto& json = response->getJsonObject();
    EXPECT_NE(json, nullptr);
    EXPECT_TRUE((*json)["ok"].asBool());
    return (*json)["data"];
}

ManagementQueryHandlers handlers(
    const std::shared_ptr<PostgresManagementQueryExecutor>& executor) {
    ManagementQueryHandlers value;
    value.list_nodes = [executor](const NodeListRequest& request) {
        return executor->list_nodes(request);
    };
    value.find_node = [executor](const std::string& node_code) {
        return executor->find_node(node_code);
    };
    value.list_data_sources =
        [executor](const NodeScopedListRequest& request) {
            return executor->list_data_sources(request);
        };
    value.list_qc_rules = [executor](const QcRuleListRequest& request) {
        return executor->list_qc_rules(request);
    };
    value.list_tasks = [executor](const NodeScopedListRequest& request) {
        return executor->list_tasks(request);
    };
    value.list_task_runs = [executor](const TaskRunListRequest& request) {
        return executor->list_task_runs(request);
    };
    value.find_task_run =
        [executor](const std::string& node_code,
                   const std::string& task_run_id) {
            return executor->find_task_run(node_code, task_run_id);
        };
    value.list_raw_files = [executor](const RunScopedListRequest& request) {
        return executor->list_raw_files(request);
    };
    value.list_parsed_records =
        [executor](const RunScopedListRequest& request) {
            return executor->list_parsed_records(request);
        };
    value.list_qc_results =
        [executor](const QcResultListRequest& request) {
            return executor->list_qc_results(request);
        };
    value.list_alerts = [executor](const AlertListRequest& request) {
        return executor->list_alerts(request);
    };
    return value;
}

ManagementCommandHandlers command_handlers(
    const std::shared_ptr<PostgresManagementCommandExecutor>& executor) {
    ManagementCommandHandlers value;
    value.create_data_source =
        [executor](const ManagementDataSourceCreateRequest& request) {
            return executor->create_data_source(request);
        };
    value.create_qc_rule =
        [executor](const ManagementQcRuleCreateRequest& request) {
            return executor->create_qc_rule(request);
        };
    value.create_task =
        [executor](const ManagementTaskCreateRequest& request) {
            return executor->create_task(request);
        };
    value.set_task_enabled =
        [executor](const std::string& task_id, bool enabled) {
            return executor->set_task_enabled(task_id, enabled);
        };
    return value;
}

class ManagementHttpPostgresTest : public testing::Test {
protected:
    void SetUp() override {
        connection_info_ = std::getenv("LABBRIDGE_DATABASE_URL");
        session_ = std::make_unique<LibpqSqlSession>(connection_info_);
        const auto transaction = session_->query_one(
            "SELECT txid_current()::text AS id", {});
        ASSERT_TRUE(transaction.has_value());
        suffix_ = value_or_empty(*transaction, "id");
        node_code_ = "phase02502-http-" + suffix_;
        rule_name_ = "phase02502-rule-" + suffix_;

        node_id_ = insert_id(
            "INSERT INTO nodes "
            "(node_code,name,status,agent_version,last_heartbeat_at) "
            "VALUES ($1,'Phase 025-02 HTTP node','online','0.1.0',now()) "
            "RETURNING id::text AS id",
            {node_code_});
        source_id_ = insert_id(
            "INSERT INTO data_sources "
            "(node_id,source_type,name,config_json,enabled) "
            "VALUES ($1::bigint,'local_directory','instrument inbox',"
            "'{\"root_path\":\"/srv/labbridge/inbox\","
            "\"extension\":\".csv\"}',true) RETURNING id::text AS id",
            {node_id_});
        rule_id_ = insert_id(
            "INSERT INTO qc_rules "
            "(name,rule_type,rule_config_json,enabled) "
            "VALUES ($1,'required_fields','{}',true) "
            "RETURNING id::text AS id",
            {rule_name_});
        task_id_ = insert_id(
            "INSERT INTO tasks "
            "(node_id,data_source_id,name,task_type,schedule_expr,"
            "parser_type,qc_profile,enabled) "
            "VALUES ($1::bigint,$2::bigint,'minute CSV import',"
            "'local_file_import','*/5 * * * *','csv_observation',"
            "'default',true) RETURNING id::text AS id",
            {node_id_, source_id_});
        session_->execute(
            "INSERT INTO task_qc_rules(task_id,qc_rule_id,sort_order) "
            "VALUES ($1::bigint,$2::bigint,10)",
            {task_id_, rule_id_});
        run_id_ = insert_id(
            "INSERT INTO task_runs "
            "(task_id,node_id,status,started_at,items_total,items_success,"
            "items_failed,trigger_type,scheduled_for) "
            "VALUES ($1::bigint,$2::bigint,'running',"
            "now()-interval '2 hours',1,1,0,'scheduled',"
            "now()-interval '2 hours') RETURNING id::text AS id",
            {task_id_, node_id_});
        raw_id_ = insert_id(
            "INSERT INTO raw_files "
            "(task_run_id,node_id,original_name,file_hash,storage_path,"
            "size_bytes,ingest_status) "
            "VALUES ($1::bigint,$2::bigint,'observation.csv','phase02502',"
            "'/archive/observation.csv',128,'archived') "
            "RETURNING id::text AS id",
            {run_id_, node_id_});
        parsed_id_ = insert_id(
            "INSERT INTO parsed_records "
            "(raw_file_id,task_run_id,station_code,device_code,record_time,"
            "payload_json,parse_status) "
            "VALUES ($1::bigint,$2::bigint,'ST025','DV025',now(),"
            "'{\"temperature\":22.5}','parsed') RETURNING id::text AS id",
            {raw_id_, run_id_});
        qc_result_id_ = insert_id(
            "INSERT INTO qc_results "
            "(parsed_record_id,qc_rule_id,level,result,message) "
            "VALUES ($1::bigint,$2::bigint,'record','passed','ok') "
            "RETURNING id::text AS id",
            {parsed_id_, rule_id_});
        alert_id_ = insert_id(
            "INSERT INTO alerts "
            "(node_id,task_run_id,alert_type,severity,message,status) "
            "VALUES ($1::bigint,$2::bigint,'run_stale','warning',"
            "'run requires observation','open') RETURNING id::text AS id",
            {node_id_, run_id_});
    }

    void TearDown() override {
        if (!session_ || node_code_.empty()) {
            return;
        }
        try {
            session_->execute(
                "DELETE FROM alerts WHERE node_id=(SELECT id FROM nodes "
                "WHERE node_code=$1)", {node_code_});
            session_->execute(
                "DELETE FROM qc_results WHERE parsed_record_id IN ("
                "SELECT pr.id FROM parsed_records pr JOIN task_runs tr "
                "ON tr.id=pr.task_run_id JOIN nodes n ON n.id=tr.node_id "
                "WHERE n.node_code=$1)", {node_code_});
            session_->execute(
                "DELETE FROM parsed_records WHERE task_run_id IN ("
                "SELECT tr.id FROM task_runs tr JOIN nodes n "
                "ON n.id=tr.node_id WHERE n.node_code=$1)", {node_code_});
            session_->execute(
                "DELETE FROM raw_files WHERE node_id=(SELECT id FROM nodes "
                "WHERE node_code=$1)", {node_code_});
            session_->execute(
                "DELETE FROM task_runs WHERE node_id=(SELECT id FROM nodes "
                "WHERE node_code=$1)", {node_code_});
            session_->execute(
                "DELETE FROM task_qc_rules WHERE task_id IN ("
                "SELECT t.id FROM tasks t JOIN nodes n ON n.id=t.node_id "
                "WHERE n.node_code=$1)", {node_code_});
            session_->execute(
                "DELETE FROM tasks WHERE node_id=(SELECT id FROM nodes "
                "WHERE node_code=$1)", {node_code_});
            session_->execute(
                "DELETE FROM data_sources WHERE node_id=(SELECT id FROM nodes "
                "WHERE node_code=$1)", {node_code_});
            for (const auto& rule_id : created_rule_ids_) {
                session_->execute(
                    "DELETE FROM qc_rules WHERE id=$1::bigint", {rule_id});
            }
            session_->execute("DELETE FROM qc_rules WHERE name=$1", {rule_name_});
            session_->execute("DELETE FROM nodes WHERE node_code=$1", {node_code_});
        } catch (const std::exception& error) {
            ADD_FAILURE() << "fixture cleanup failed: " << error.what();
        }
    }

    std::string insert_id(const std::string& sql, const SqlParams& params) {
        const auto row = session_->query_one(sql, params);
        EXPECT_TRUE(row.has_value());
        return row.has_value() ? value_or_empty(*row, "id") : "";
    }

    std::string connection_info_;
    std::unique_ptr<LibpqSqlSession> session_;
    std::string suffix_;
    std::string node_code_;
    std::string rule_name_;
    std::string node_id_;
    std::string source_id_;
    std::string rule_id_;
    std::string task_id_;
    std::string run_id_;
    std::string raw_id_;
    std::string parsed_id_;
    std::string qc_result_id_;
    std::string alert_id_;
    std::vector<std::string> created_rule_ids_;
};

TEST_F(ManagementHttpPostgresTest, ReadsCompleteManagementEvidenceThroughHttpDtos) {
    auto executor = std::make_shared<PostgresManagementQueryExecutor>(
        connection_info_, 600, 3600);
    auto command_executor =
        std::make_shared<PostgresManagementCommandExecutor>(connection_info_);
    ManagementHttpController controller{
        handlers(executor), command_handlers(command_executor)};

    const auto nodes = data(invoke([&](auto callback) {
        controller.get_nodes(request({{"limit", "100"}}),
                             std::move(callback));
    }));
    bool found_node = false;
    for (const auto& node : nodes["items"]) {
        found_node = found_node || node["node_code"].asString() == node_code_;
    }
    EXPECT_TRUE(found_node);

    const auto node = data(invoke([&](auto callback) {
        controller.get_node(request(), node_code_, std::move(callback));
    }));
    EXPECT_EQ(node["effective_status"].asString(), "online");
    EXPECT_EQ(node["enabled_task_count"].asInt(), 1);

    const auto sources = data(invoke([&](auto callback) {
        controller.get_data_sources(request({{"node_code", node_code_}}),
                                    std::move(callback));
    }));
    EXPECT_EQ(sources["items"][0]["id"].asString(), source_id_);
    EXPECT_EQ(sources["items"][0]["config"]["extension"].asString(), ".csv");

    const auto rules = data(invoke([&](auto callback) {
        controller.get_qc_rules(request({{"limit", "100"}}),
                                std::move(callback));
    }));
    bool found_rule = false;
    for (const auto& rule : rules["items"]) {
        found_rule = found_rule || rule["id"].asString() == rule_id_;
    }
    EXPECT_TRUE(found_rule);

    const auto tasks = data(invoke([&](auto callback) {
        controller.get_tasks(request({{"node_code", node_code_}}),
                             std::move(callback));
    }));
    EXPECT_EQ(tasks["items"][0]["qc_rule_ids"][0].asString(), rule_id_);

    const auto runs = data(invoke([&](auto callback) {
        controller.get_task_runs(request({{"node_code", node_code_},
                                           {"task_id", task_id_},
                                           {"status", "running"}}),
                                  std::move(callback));
    }));
    EXPECT_TRUE(runs["items"][0]["stale"].asBool());

    const auto run = data(invoke([&](auto callback) {
        controller.get_task_run(request({{"node_code", node_code_}}),
                                run_id_, std::move(callback));
    }));
    EXPECT_EQ(run["raw_file_count"].asInt(), 1);
    EXPECT_EQ(run["parsed_record_count"].asInt(), 1);
    EXPECT_EQ(run["qc_result_count"].asInt(), 1);
    EXPECT_EQ(run["alert_count"].asInt(), 1);

    const auto raw = data(invoke([&](auto callback) {
        controller.get_raw_files(request({{"task_run_id", run_id_}}),
                                 std::move(callback));
    }));
    const auto parsed = data(invoke([&](auto callback) {
        controller.get_parsed_records(request({{"task_run_id", run_id_}}),
                                      std::move(callback));
    }));
    const auto qc = data(invoke([&](auto callback) {
        controller.get_qc_results(request({{"task_run_id", run_id_},
                                            {"result", "passed"}}),
                                  std::move(callback));
    }));
    const auto alerts = data(invoke([&](auto callback) {
        controller.get_alerts(request({{"node_code", node_code_},
                                       {"task_run_id", run_id_},
                                       {"status", "open"},
                                       {"severity", "warning"}}),
                              std::move(callback));
    }));

    EXPECT_EQ(raw["items"][0]["id"].asString(), raw_id_);
    EXPECT_EQ(parsed["items"][0]["raw_file_id"].asString(), raw_id_);
    EXPECT_DOUBLE_EQ(parsed["items"][0]["payload"]["temperature"].asDouble(),
                     22.5);
    EXPECT_EQ(qc["items"][0]["id"].asString(), qc_result_id_);
    EXPECT_EQ(alerts["items"][0]["id"].asString(), alert_id_);

    std::cout << "management_http node=" << node_code_
              << " effective_status=online source=" << source_id_
              << " task=" << task_id_ << " qc_rule=" << rule_id_
              << " run=" << run_id_
              << " stale=true raw=1 parsed=1 qc=1 alerts=1"
              << " payload.temperature=22.5" << std::endl;
}

TEST_F(ManagementHttpPostgresTest,
       WritesTransactionalConfigurationThroughHttpDtos) {
    auto query_executor = std::make_shared<PostgresManagementQueryExecutor>(
        connection_info_, 600, 3600);
    auto command_executor =
        std::make_shared<PostgresManagementCommandExecutor>(connection_info_);
    ManagementHttpController controller{
        handlers(query_executor), command_handlers(command_executor)};

    Json::Value source_body;
    source_body["node_code"] = node_code_;
    source_body["source_type"] = "local_directory";
    source_body["name"] = "phase02504-source-" + suffix_;
    source_body["config"]["root_path"] = "/tmp/phase02504-inbox";
    source_body["config"]["extension"] = ".csv";
    source_body["enabled"] = true;
    const auto source = success_data(invoke([&](auto callback) {
        controller.post_data_source(
            json_request(drogon::Post, source_body), std::move(callback));
    }), drogon::k201Created);
    const auto created_source_id = source["id"].asString();
    EXPECT_TRUE(source["config"].isObject());
    EXPECT_TRUE(source["created_at"].isString());

    const auto create_rule = [&](const std::string& type, bool enabled) {
        Json::Value rule_body;
        rule_body["name"] = "phase02504-" + type + "-" + suffix_;
        rule_body["rule_type"] = type;
        rule_body["config"] = Json::Value{Json::objectValue};
        rule_body["enabled"] = enabled;
        const auto rule = success_data(invoke([&](auto callback) {
            controller.post_qc_rule(
                json_request(drogon::Post, rule_body), std::move(callback));
        }), drogon::k201Created);
        const auto id = rule["id"].asString();
        created_rule_ids_.push_back(id);
        return id;
    };
    const auto timestamp_rule_id =
        create_rule("basic_timestamp_format", true);
    const auto required_rule_id = create_rule("required_fields", true);
    const auto disabled_rule_id = create_rule("required_fields", false);

    Json::Value task_body;
    task_body["node_code"] = node_code_;
    task_body["data_source_id"] = created_source_id;
    task_body["name"] = "phase02504-task-" + suffix_;
    task_body["task_type"] = "local_file_import";
    task_body["schedule_expr"] = "*/5 * * * *";
    task_body["parser_type"] = "csv_observation";
    task_body["qc_profile"] = "default";
    task_body["qc_rule_ids"] = Json::Value{Json::arrayValue};
    task_body["qc_rule_ids"].append(timestamp_rule_id);
    task_body["qc_rule_ids"].append(required_rule_id);
    task_body["enabled"] = true;
    const auto task = success_data(invoke([&](auto callback) {
        controller.post_task(
            json_request(drogon::Post, task_body), std::move(callback));
    }), drogon::k201Created);
    const auto created_task_id = task["id"].asString();
    ASSERT_EQ(task["qc_rule_ids"].size(), 2U);
    EXPECT_EQ(task["qc_rule_ids"][0].asString(), timestamp_rule_id);
    EXPECT_EQ(task["qc_rule_ids"][1].asString(), required_rule_id);
    EXPECT_TRUE(task["created_at"].isString());

    Json::Value patch_body;
    patch_body["enabled"] = false;
    const auto patched = success_data(invoke([&](auto callback) {
        controller.patch_task(
            json_request(drogon::Patch, patch_body), created_task_id,
            std::move(callback));
    }), drogon::k200OK);
    EXPECT_FALSE(patched["enabled"].asBool());
    EXPECT_TRUE(patched["updated_at"].isString());

    const auto rejected_task_name = "phase02504-rejected-" + suffix_;
    task_body["name"] = rejected_task_name;
    task_body["qc_rule_ids"] = Json::Value{Json::arrayValue};
    task_body["qc_rule_ids"].append(disabled_rule_id);
    const auto rejected = invoke([&](auto callback) {
        controller.post_task(
            json_request(drogon::Post, task_body), std::move(callback));
    });
    EXPECT_EQ(rejected->statusCode(), drogon::k409Conflict);
    ASSERT_NE(rejected->getJsonObject(), nullptr);
    EXPECT_EQ((*rejected->getJsonObject())["error"]["code"].asString(),
              "conflict");

    const auto duplicate_task_name = "phase02504-duplicate-" + suffix_;
    task_body["name"] = duplicate_task_name;
    task_body["qc_rule_ids"] = Json::Value{Json::arrayValue};
    task_body["qc_rule_ids"].append(required_rule_id);
    task_body["qc_rule_ids"].append(required_rule_id);
    const auto duplicate = invoke([&](auto callback) {
        controller.post_task(
            json_request(drogon::Post, task_body), std::move(callback));
    });
    EXPECT_EQ(duplicate->statusCode(), drogon::k400BadRequest);
    ASSERT_NE(duplicate->getJsonObject(), nullptr);
    EXPECT_EQ((*duplicate->getJsonObject())["error"]["code"].asString(),
              "invalid_argument");

    const auto persisted = session_->query_one(
        "SELECT CASE WHEN t.enabled THEN 'true' ELSE 'false' END AS enabled, "
        "string_agg(tqr.qc_rule_id::text, ',' ORDER BY tqr.sort_order) "
        "AS bindings FROM tasks t JOIN task_qc_rules tqr "
        "ON tqr.task_id=t.id WHERE t.id=$1::bigint GROUP BY t.id",
        {created_task_id});
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(value_or_empty(*persisted, "enabled"), "false");
    EXPECT_EQ(value_or_empty(*persisted, "bindings"),
              timestamp_rule_id + "," + required_rule_id);
    const auto rejected_count = session_->query_one(
        "SELECT count(*)::text AS count FROM tasks WHERE name=$1",
        {rejected_task_name});
    ASSERT_TRUE(rejected_count.has_value());
    EXPECT_EQ(value_or_empty(*rejected_count, "count"), "0");
    const auto duplicate_count = session_->query_one(
        "SELECT count(*)::text AS count FROM tasks WHERE name=$1",
        {duplicate_task_name});
    ASSERT_TRUE(duplicate_count.has_value());
    EXPECT_EQ(value_or_empty(*duplicate_count, "count"), "0");

    std::cout << "management_command_http source=" << created_source_id
              << " task=" << created_task_id
              << " bindings=" << timestamp_rule_id << "," << required_rule_id
              << " create_status=201 patch_enabled=false"
              << " rejected_status=409 rejected_tasks=0"
              << " duplicate_status=400 duplicate_tasks=0" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    const char* connection = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection == nullptr || std::string{connection}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping "
                     "management HTTP PostgreSQL test\n";
        return 77;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
