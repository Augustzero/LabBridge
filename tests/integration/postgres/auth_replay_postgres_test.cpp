#include "labbridge/server/http/agent_report_http_controller.h"
#include "labbridge/server/http/http_authenticator.h"
#include "labbridge/server/http/task_run_http_controller.h"
#include "labbridge/server/postgres/agent_report_executor.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/storage_mapping.h"
#include "labbridge/server/postgres/task_run_executor.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <json/writer.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace labbridge::server;
using labbridge::server::storage::value_or_empty;

const std::string kNode = "auth-replay-node";
const std::string kInitialToken(64, 'a');
// 密钥轮换后的新 token：旧 token 随之失效。
const std::string kRotatedToken(64, 'c');
const std::string kExecutionKey = "auth-replay-execution";

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

std::map<std::string, long long> business_row_counts(LibpqSqlSession& session) {
    const auto rows = session.query_all(
        "SELECT 'nodes' AS entity, count(*)::bigint AS count FROM nodes "
        "UNION ALL SELECT 'data_sources', count(*) FROM data_sources "
        "UNION ALL SELECT 'tasks', count(*) FROM tasks "
        "UNION ALL SELECT 'task_runs', count(*) FROM task_runs "
        "UNION ALL SELECT 'raw_files', count(*) FROM raw_files "
        "UNION ALL SELECT 'parsed_records', count(*) FROM parsed_records "
        "UNION ALL SELECT 'qc_results', count(*) FROM qc_results "
        "UNION ALL SELECT 'alerts', count(*) FROM alerts "
        "UNION ALL SELECT 'agent_report_receipts', count(*) "
        "  FROM agent_report_receipts",
        {});
    std::map<std::string, long long> counts;
    for (const auto& row : rows) {
        counts[value_or_empty(row, "entity")] =
            std::stoll(value_or_empty(row, "count"));
    }
    return counts;
}

class AuthReplayPostgresTest : public testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("LABBRIDGE_DATABASE_URL");
        ASSERT_NE(url, nullptr);
        ASSERT_FALSE(std::string(url).empty());
        connection_info_ = url;
        session_ = std::make_unique<LibpqSqlSession>(connection_info_);

        seed_business_fixture();
        api_ = build_api(kInitialToken);
    }

    void TearDown() override {
        if (!session_) {
            return;
        }
        try {
            cleanup_business_fixture();
        } catch (const std::exception& error) {
            ADD_FAILURE() << "fixture cleanup failed: " << error.what();
        }
    }

    // 节点 + 数据源 + 任务 + QC 规则，给闭环准备可归属的业务对象。
    void seed_business_fixture() {
        insert("INSERT INTO nodes (node_code,name,status,agent_version) "
               "VALUES ($1,'auth replay node','online','0.1.0')", {kNode});
        data_source_id_ = insert_returning_id(
            "INSERT INTO data_sources "
            "(node_id,source_type,name,config_json,enabled) "
            "SELECT id,'local_directory','auth replay source',"
            "'{\"root_path\":\"/demo/inbox\",\"extension\":\".csv\"}',true "
            "FROM nodes WHERE node_code=$1", {kNode});
        task_id_ = insert_returning_id(
            "INSERT INTO tasks "
            "(node_id,data_source_id,name,task_type,schedule_expr,"
            "parser_type,enabled) "
            "SELECT id,$1::bigint,'auth replay task','local_file_import',"
            "'* * * * *','csv_observation',true "
            "FROM nodes WHERE node_code=$2", {data_source_id_, kNode});
        qc_rule_id_ = insert_returning_id(
            "INSERT INTO qc_rules (name,rule_type,rule_config_json,enabled) "
            "VALUES ('auth replay rule','required_fields','{}',true)", {});
        insert("INSERT INTO task_qc_rules (task_id,qc_rule_id,sort_order) "
               "VALUES ($1::bigint,$2::bigint,0)",
               {task_id_, qc_rule_id_});
    }

    // 依赖顺序逐表清理：schema 没有级联删除，顺序反了会被外键挡住。
    void cleanup_business_fixture() {
        insert("DELETE FROM alerts WHERE node_id="
               "(SELECT id FROM nodes WHERE node_code=$1)", {kNode});
        insert("DELETE FROM qc_results WHERE parsed_record_id IN "
               "(SELECT pr.id FROM parsed_records pr "
               " JOIN task_runs tr ON tr.id=pr.task_run_id "
               " JOIN nodes n ON n.id=tr.node_id WHERE n.node_code=$1)",
               {kNode});
        insert("DELETE FROM parsed_records WHERE task_run_id IN "
               "(SELECT tr.id FROM task_runs tr "
               " JOIN nodes n ON n.id=tr.node_id WHERE n.node_code=$1)",
               {kNode});
        insert("DELETE FROM agent_report_receipts WHERE node_id="
               "(SELECT id FROM nodes WHERE node_code=$1)", {kNode});
        insert("DELETE FROM raw_files WHERE node_id="
               "(SELECT id FROM nodes WHERE node_code=$1)", {kNode});
        insert("DELETE FROM task_runs WHERE node_id="
               "(SELECT id FROM nodes WHERE node_code=$1)", {kNode});
        insert("DELETE FROM task_qc_rules WHERE task_id IN "
               "(SELECT t.id FROM tasks t "
               " JOIN nodes n ON n.id=t.node_id WHERE n.node_code=$1)",
               {kNode});
        insert("DELETE FROM tasks WHERE node_id="
               "(SELECT id FROM nodes WHERE node_code=$1)", {kNode});
        insert("DELETE FROM data_sources WHERE node_id="
               "(SELECT id FROM nodes WHERE node_code=$1)", {kNode});
        insert("DELETE FROM nodes WHERE node_code=$1", {kNode});
        if (!qc_rule_id_.empty()) {
            insert("DELETE FROM qc_rules WHERE id=$1::bigint", {qc_rule_id_});
        }
    }

    struct AuthenticatedApi {
        std::unique_ptr<TaskRunHttpController> task_runs;
        std::unique_ptr<AgentReportHttpController> reports;
    };

    // 同一组 executor，换认证对象即等价于"改凭据文件后重启 Server"。
    AuthenticatedApi build_api(const std::string& node_token) {
        auto authenticator = std::make_shared<HttpAuthenticator>(
            HttpAuthenticator::CredentialSet{
                std::string(64, '1'), {{kNode, node_token}}});
        auto report_executor =
            std::make_shared<PostgresAgentReportExecutor>(connection_info_);
        auto task_run_executor =
            std::make_shared<PostgresTaskRunExecutor>(connection_info_);
        AuthenticatedApi api;
        api.task_runs = std::make_unique<TaskRunHttpController>(
            authenticator,
            [task_run_executor](const StartTaskRunRequest& request) {
                return task_run_executor->start(request);
            });
        api.reports = std::make_unique<AgentReportHttpController>(
            authenticator,
            [report_executor](const RawFileManifestRequest& request) {
                return report_executor->accept_raw_file_manifest(request);
            },
            [report_executor](const TaskRunReportRequest& request) {
                return report_executor->accept_task_run_report(request);
            });
        return api;
    }

    drogon::HttpRequestPtr node_request(drogon::HttpMethod method,
                                        const std::string& body,
                                        const std::string& token) const {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(method);
        if (!body.empty()) {
            request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            request->setBody(body);
        }
        request->addHeader("Authorization", "Bearer " + token);
        request->addHeader("X-LabBridge-Node-Code", kNode);
        return request;
    }

    template <typename Operation>
    static drogon::HttpResponsePtr invoke(Operation operation) {
        drogon::HttpResponsePtr response;
        operation([&response](const drogon::HttpResponsePtr& current) {
            response = current;
        });
        EXPECT_NE(response, nullptr);
        return response;
    }

    static std::string data_string(const drogon::HttpResponsePtr& response,
                                   const std::string& field) {
        return (*response->getJsonObject())["data"][field].asString();
    }

    void insert(const std::string& sql, const SqlParams& params) {
        session_->execute(sql, params);
    }

    std::string insert_returning_id(const std::string& sql,
                                    const SqlParams& params) {
        const auto row = session_->query_one(
            sql + " RETURNING id::text AS id", params);
        if (!row.has_value()) {
            throw std::runtime_error("seed insert returned no id");
        }
        return value_or_empty(*row, "id");
    }

    std::string connection_info_;
    std::unique_ptr<LibpqSqlSession> session_;
    AuthenticatedApi api_;
    std::string data_source_id_;
    std::string task_id_;
    std::string qc_rule_id_;
};

TEST_F(AuthReplayPostgresTest, RotatedCredentialReplayKeepsSingleEvidence) {
    // 闭环第一段：初始密钥走完 start -> manifest -> report。
    Json::Value start_body;
    start_body["node_code"] = kNode;
    start_body["task_id"] = task_id_;
    start_body["execution_key"] = kExecutionKey;
    start_body["scheduled_for"] = "2026-09-11T00:00:00Z";
    start_body["started_at"] = "2026-09-11T00:00:01Z";
    start_body["trigger_type"] = "scheduled";

    const auto start_response = invoke([&](auto callback) {
        api_.task_runs->post_start(
            node_request(drogon::Post, write_json(start_body), kInitialToken),
            std::move(callback));
    });
    ASSERT_EQ(start_response->statusCode(), drogon::k201Created);
    const std::string run_id = data_string(start_response, "task_run_id");
    ASSERT_TRUE(run_id.find_first_not_of("0123456789") == std::string::npos);

    Json::Value manifest_body;
    manifest_body["task_run_id"] = run_id;
    manifest_body["node_code"] = kNode;
    manifest_body["idempotency_key"] = "auth-replay-manifest-" + run_id;
    Json::Value file;
    file["original_name"] = "auth_replay_observation.csv";
    file["file_hash"] = "auth-replay-file-hash";
    file["storage_path"] = "/archive/auth-replay/auth_replay_observation.csv";
    file["size_bytes"] = Json::Int64{128};
    file["source_mtime"] = "2026-09-11 08:00:00+08";
    file["ingest_status"] = "archived";
    manifest_body["files"].append(std::move(file));

    const auto manifest_response = invoke([&](auto callback) {
        api_.reports->post_raw_file_manifest(
            node_request(drogon::Post, write_json(manifest_body),
                         kInitialToken),
            std::move(callback));
    });
    ASSERT_EQ(manifest_response->statusCode(), drogon::k201Created);
    const auto& manifest_json = *manifest_response->getJsonObject();
    ASSERT_EQ(manifest_json["data"]["raw_file_ids"].size(), 1U);
    const std::string raw_file_id =
        manifest_json["data"]["raw_file_ids"][Json::ArrayIndex{0}].asString();

    Json::Value report_body;
    report_body["task_run_id"] = run_id;
    report_body["node_code"] = kNode;
    report_body["idempotency_key"] = "auth-replay-report-" + run_id;
    report_body["status"] = "failed";
    report_body["finished_at"] = "2026-09-11 08:03:00+08";
    report_body["items_total"] = 1;
    report_body["items_success"] = 0;
    report_body["items_failed"] = 1;
    report_body["error_summary"] = "qc failed";
    Json::Value parsed;
    parsed["raw_file_id"] = raw_file_id;
    parsed["station_code"] = "station-replay";
    parsed["device_code"] = "device-replay";
    parsed["record_time"] = "2026-09-11 08:00:00+08";
    parsed["payload_json"] = "{\"temperature\":48.5}";
    parsed["parse_status"] = "parsed";
    Json::Value passed;
    passed["qc_rule_id"] = qc_rule_id_;
    passed["level"] = "pass";
    passed["result"] = "passed";
    passed["message"] = "required fields present";
    parsed["qc_results"].append(std::move(passed));
    Json::Value failed;
    failed["qc_rule_id"] = qc_rule_id_;
    failed["level"] = "failed";
    failed["result"] = "failed";
    failed["message"] = "value outside configured range";
    parsed["qc_results"].append(std::move(failed));
    report_body["parsed_records"].append(std::move(parsed));

    const auto report_response = invoke([&](auto callback) {
        api_.reports->post_task_run_report(
            node_request(drogon::Post, write_json(report_body), kInitialToken),
            std::move(callback));
    });
    ASSERT_EQ(report_response->statusCode(), drogon::k200OK);
    const auto& report_json = *report_response->getJsonObject();
    const std::string parsed_record_id =
        report_json["data"]["parsed_record_ids"][Json::ArrayIndex{0}]
            .asString();
    ASSERT_EQ(report_json["data"]["qc_result_ids"].size(), 2U);
    ASSERT_EQ(report_json["data"]["alert_ids"].size(), 1U);

    // SQL 回验闭环证据链：run -> raw -> parsed，QC 与告警挂在正确归属下。
    const auto chain = session_->query_one(
        "SELECT tr.id::text AS run_id, tr.status, n.node_code, "
        "rf.id::text AS raw_file_id, rf.storage_path, "
        "pr.id::text AS parsed_record_id, "
        "(SELECT count(*) FROM qc_results qr "
        " WHERE qr.parsed_record_id=pr.id)::text AS qc_count, "
        "(SELECT count(*) FROM alerts al "
        " WHERE al.task_run_id=tr.id)::text AS alert_count "
        "FROM task_runs tr "
        "JOIN nodes n ON n.id=tr.node_id "
        "JOIN raw_files rf ON rf.task_run_id=tr.id "
        "JOIN parsed_records pr ON pr.raw_file_id=rf.id "
        "WHERE tr.execution_key=$1 AND n.node_code=$2",
        {kExecutionKey, kNode});
    ASSERT_TRUE(chain.has_value());
    EXPECT_EQ(value_or_empty(*chain, "run_id"), run_id);
    EXPECT_EQ(value_or_empty(*chain, "status"), "failed");
    EXPECT_EQ(value_or_empty(*chain, "node_code"), kNode);
    EXPECT_EQ(value_or_empty(*chain, "raw_file_id"), raw_file_id);
    EXPECT_EQ(value_or_empty(*chain, "parsed_record_id"), parsed_record_id);
    EXPECT_EQ(value_or_empty(*chain, "qc_count"), "2");
    EXPECT_EQ(value_or_empty(*chain, "alert_count"), "1");

    // 回执核对：节点、请求类型、幂等键、已完成。
    const auto receipts = session_->query_all(
        "SELECT n.node_code, ar.request_type, ar.idempotency_key, "
        "(ar.completed_at IS NOT NULL)::text AS completed "
        "FROM agent_report_receipts ar "
        "JOIN nodes n ON n.id=ar.node_id "
        "WHERE ar.task_run_id=$1::bigint ORDER BY ar.request_type",
        {run_id});
    ASSERT_EQ(receipts.size(), 2U);
    EXPECT_EQ(value_or_empty(receipts[0], "node_code"), kNode);
    EXPECT_EQ(value_or_empty(receipts[0], "request_type"),
              "raw_file_manifest");
    EXPECT_EQ(value_or_empty(receipts[0], "idempotency_key"),
              "auth-replay-manifest-" + run_id);
    EXPECT_EQ(value_or_empty(receipts[0], "completed"), "true");
    EXPECT_EQ(value_or_empty(receipts[1], "request_type"), "task_run_report");
    EXPECT_EQ(value_or_empty(receipts[1], "idempotency_key"),
              "auth-replay-report-" + run_id);
    EXPECT_EQ(value_or_empty(receipts[1], "completed"), "true");

    const auto before = business_row_counts(*session_);

    // 密钥轮换：相当于 Server 侧改凭据文件后重启。
    api_ = build_api(kRotatedToken);

    // 新密钥重放 start：同一个 execution_key 必须命中同一条 run。
    const auto replay_start = invoke([&](auto callback) {
        api_.task_runs->post_start(
            node_request(drogon::Post, write_json(start_body), kRotatedToken),
            std::move(callback));
    });
    ASSERT_EQ(replay_start->statusCode(), drogon::k201Created);
    EXPECT_EQ(data_string(replay_start, "task_run_id"), run_id);
    EXPECT_TRUE((*replay_start->getJsonObject())["data"]["replayed"].asBool());

    // 新密钥重放 manifest：返回既有 raw_file_ids，不新增行。
    const auto replay_manifest = invoke([&](auto callback) {
        api_.reports->post_raw_file_manifest(
            node_request(drogon::Post, write_json(manifest_body),
                         kRotatedToken),
            std::move(callback));
    });
    ASSERT_EQ(replay_manifest->statusCode(), drogon::k201Created);
    const auto& replay_manifest_json = *replay_manifest->getJsonObject();
    ASSERT_EQ(replay_manifest_json["data"]["raw_file_ids"].size(), 1U);
    EXPECT_EQ(replay_manifest_json["data"]["raw_file_ids"][Json::ArrayIndex{0}]
                  .asString(),
              raw_file_id);
    EXPECT_TRUE(replay_manifest_json["data"]["replayed"].asBool());

    // 新密钥重放 report：同一批 ID 回来，业务表行数不变。
    const auto replay_report = invoke([&](auto callback) {
        api_.reports->post_task_run_report(
            node_request(drogon::Post, write_json(report_body), kRotatedToken),
            std::move(callback));
    });
    ASSERT_EQ(replay_report->statusCode(), drogon::k200OK);
    const auto& replay_report_json = *replay_report->getJsonObject();
    ASSERT_EQ(replay_report_json["data"]["parsed_record_ids"].size(), 1U);
    EXPECT_EQ(replay_report_json["data"]["parsed_record_ids"]
                  [Json::ArrayIndex{0}]
                      .asString(),
              parsed_record_id);
    EXPECT_EQ(replay_report_json["data"]["qc_result_ids"],
              report_json["data"]["qc_result_ids"]);
    EXPECT_EQ(replay_report_json["data"]["alert_ids"],
              report_json["data"]["alert_ids"]);
    EXPECT_TRUE(replay_report_json["data"]["replayed"].asBool());

    EXPECT_EQ(business_row_counts(*session_), before);

    // 被撤销的旧密钥：401 且零写入。
    const auto stale_response = invoke([&](auto callback) {
        api_.reports->post_raw_file_manifest(
            node_request(drogon::Post, write_json(manifest_body),
                         kInitialToken),
            std::move(callback));
    });
    ASSERT_EQ(stale_response->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(business_row_counts(*session_), before);

    std::cout << "auth_replay run=" << run_id << " raw=" << raw_file_id
              << " parsed=" << parsed_record_id
              << " replay_with_rotated_token=no_new_rows=true"
              << " stale_token=401" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    const char* connection = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection == nullptr || std::string{connection}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping "
                     "auth replay PostgreSQL test\n";
        return 77;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
