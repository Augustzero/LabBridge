#include "labbridge/server/application/management_query_service.h"
#include "labbridge/server/postgres/libpq_sql_session.h"
#include "labbridge/server/postgres/management_query_repository.h"
#include "labbridge/server/postgres/storage_mapping.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using labbridge::server::LibpqSqlSession;
using labbridge::server::ManagementQueryService;
using labbridge::server::PostgresManagementQueryRepository;
using labbridge::server::storage::value_or_empty;

void execute_sql_file(
    LibpqSqlSession& session,
    const std::string& path) {
    std::ifstream input{path};
    ASSERT_TRUE(input.is_open()) << path;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::istringstream statements{buffer.str()};
    std::string statement;
    while (std::getline(statements, statement, ';')) {
        if (statement.find_first_not_of(" \n\r\t") !=
            std::string::npos) {
            session.execute(statement, {});
        }
    }
}

class ManagementQueryPostgresTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<LibpqSqlSession>(
            std::getenv("LABBRIDGE_DATABASE_URL"));
        session_->execute("BEGIN", {});
        active_ = true;
        const auto tx = session_->query_one(
            "SELECT txid_current()::text AS id", {});
        ASSERT_TRUE(tx.has_value());
        node_code_ = "phase02501-" + value_or_empty(*tx, "id");

        node_id_ = insert_id(
            "INSERT INTO nodes "
            "(node_code,name,status,agent_version,last_heartbeat_at) "
            "VALUES ($1,'Phase 025-01','online','0.1.0',"
            "'2026-08-19T00:00:00Z') RETURNING id::text AS id",
            {node_code_});

        for (int index = 1; index <= 3; ++index) {
            source_ids_.push_back(insert_id(
                "INSERT INTO data_sources "
                "(node_id,source_type,name,config_json,enabled) "
                "VALUES ($1::bigint,'local_directory',$2,"
                "jsonb_build_object('root_path',$3::text,'extension','.csv'),"
                "$4::boolean) RETURNING id::text AS id",
                {
                    node_id_,
                    "source-" + std::to_string(index),
                    "/tmp/phase02501/" + std::to_string(index),
                    index == 2 ? "false" : "true",
                }));
            task_ids_.push_back(insert_id(
                "INSERT INTO tasks "
                "(node_id,data_source_id,name,task_type,schedule_expr,"
                "parser_type,qc_profile,enabled) "
                "VALUES ($1::bigint,$2::bigint,$3,"
                "'local_file_import','* * * * *','csv_observation',"
                "'phase02501',$4::boolean) "
                "RETURNING id::text AS id",
                {
                    node_id_,
                    source_ids_.back(),
                    "task-" + std::to_string(index),
                    index == 2 ? "false" : "true",
                }));
        }

        for (int index = 1; index <= 2; ++index) {
            rule_ids_.push_back(insert_id(
                "INSERT INTO qc_rules "
                "(name,rule_type,rule_config_json,enabled) "
                "VALUES ($1,'required_fields','{}',true) "
                "RETURNING id::text AS id",
                {"phase02501-rule-" + std::to_string(index)}));
        }
        session_->execute(
            "INSERT INTO task_qc_rules(task_id,qc_rule_id,sort_order) "
            "VALUES ($1::bigint,$2::bigint,20),"
            "($1::bigint,$3::bigint,10)",
            {task_ids_[2], rule_ids_[0], rule_ids_[1]});

        run_id_ = insert_id(
            "INSERT INTO task_runs "
            "(task_id,node_id,status,started_at,items_total,"
            "items_success,items_failed,trigger_type,scheduled_for) "
            "VALUES ($1::bigint,$2::bigint,'running',"
            "'2026-08-18T22:00:00Z',2,1,1,'scheduled',"
            "'2026-08-18T22:00:00Z') RETURNING id::text AS id",
            {task_ids_[2], node_id_});
        raw_id_ = insert_id(
            "INSERT INTO raw_files "
            "(task_run_id,node_id,original_name,file_hash,storage_path,"
            "size_bytes,ingest_status) "
            "VALUES ($1::bigint,$2::bigint,'observations.csv','hash',"
            "'/archive/observations.csv',128,'archived') "
            "RETURNING id::text AS id",
            {run_id_, node_id_});
        parsed_id_ = insert_id(
            "INSERT INTO parsed_records "
            "(raw_file_id,task_run_id,station_code,device_code,"
            "record_time,payload_json,parse_status) "
            "VALUES ($1::bigint,$2::bigint,'ST001','DV001',"
            "'2026-08-18T22:01:00Z',"
            "jsonb_build_object('temperature',22.5),'parsed') "
            "RETURNING id::text AS id",
            {raw_id_, run_id_});
        insert_id(
            "INSERT INTO qc_results "
            "(parsed_record_id,qc_rule_id,level,result,message) "
            "VALUES ($1::bigint,$2::bigint,'record','failed',"
            "'required field missing') RETURNING id::text AS id",
            {parsed_id_, rule_ids_[0]});
        insert_id(
            "INSERT INTO alerts "
            "(node_id,task_run_id,alert_type,severity,message,status) "
            "VALUES ($1::bigint,$2::bigint,'qc_failed','failed',"
            "'required field missing','open') "
            "RETURNING id::text AS id",
            {node_id_, run_id_});
    }

    void TearDown() override {
        if (active_) {
            try {
                session_->execute("ROLLBACK", {});
            } catch (const std::exception& error) {
                ADD_FAILURE() << error.what();
            }
        }
    }

    std::string insert_id(
        const std::string& sql,
        const labbridge::server::SqlParams& params) {
        const auto row = session_->query_one(sql, params);
        EXPECT_TRUE(row.has_value());
        return row.has_value() ? value_or_empty(*row, "id") : "";
    }

    LibpqSqlSession& session() {
        return *session_;
    }

    std::unique_ptr<LibpqSqlSession> session_;
    bool active_{false};
    std::string node_code_;
    std::string node_id_;
    std::vector<std::string> source_ids_;
    std::vector<std::string> task_ids_;
    std::vector<std::string> rule_ids_;
    std::string run_id_;
    std::string raw_id_;
    std::string parsed_id_;
};

TEST_F(ManagementQueryPostgresTest,
       MigrationIsRepeatableAndDefinesEveryManagementIndex) {
    execute_sql_file(
        session(),
        "deploy/migrations/025_01_management_query_indexes.sql");
    execute_sql_file(
        session(),
        "deploy/migrations/025_01_management_query_indexes.sql");
    const auto indexes = session().query_one(
        "SELECT count(*)::text AS count FROM pg_indexes "
        "WHERE schemaname=current_schema() AND indexname IN ("
        "'data_sources_node_management_idx',"
        "'tasks_node_management_idx',"
        "'task_runs_node_management_idx',"
        "'task_runs_task_management_idx',"
        "'raw_files_task_run_management_idx',"
        "'parsed_records_task_run_management_idx',"
        "'qc_results_parsed_record_management_idx',"
        "'alerts_node_management_idx',"
        "'alerts_task_run_management_idx')",
        {});
    ASSERT_TRUE(indexes.has_value());
    EXPECT_EQ(value_or_empty(*indexes, "count"), "9");
    std::cout << "management_indexes=9 migration_replay=ok"
              << std::endl;
}

TEST_F(ManagementQueryPostgresTest,
       KeysetPageExcludesConcurrentInsertAndShowsDisabledConfig) {
    PostgresManagementQueryRepository repository{session()};
    ManagementQueryService service{
        repository,
        std::chrono::system_clock::time_point{
            std::chrono::seconds{1787097600}},
        60,
        3600};

    const auto first = service.list_tasks(
        {node_code_, std::nullopt, {2, std::nullopt}});
    ASSERT_TRUE(first.status.ok);
    ASSERT_EQ(first.page.items.size(), 2U);
    ASSERT_TRUE(first.page.next_cursor.has_value());
    EXPECT_EQ(first.page.items[0].qc_rule_ids,
              (std::vector<std::string>{
                  rule_ids_[1], rule_ids_[0]}));

    const auto new_id = insert_id(
        "INSERT INTO tasks "
        "(node_id,data_source_id,name,task_type,schedule_expr,"
        "parser_type,qc_profile,enabled) "
        "VALUES ($1::bigint,$2::bigint,'concurrent-new',"
        "'local_file_import','* * * * *','csv_observation',"
        "'phase02501',true) RETURNING id::text AS id",
        {node_id_, source_ids_[0]});
    const auto second = service.list_tasks(
        {node_code_, std::nullopt,
         {2, first.page.next_cursor}});

    ASSERT_TRUE(second.status.ok);
    ASSERT_EQ(second.page.items.size(), 1U);
    EXPECT_NE(second.page.items.front().id, new_id);
    EXPECT_NE(second.page.items.front().id,
              first.page.items[0].id);
    EXPECT_NE(second.page.items.front().id,
              first.page.items[1].id);

    const auto disabled = service.list_data_sources(
        {node_code_, false, {20, std::nullopt}});
    ASSERT_TRUE(disabled.status.ok);
    ASSERT_EQ(disabled.page.items.size(), 1U);
    EXPECT_EQ(disabled.page.items.front().id, source_ids_[1]);
    std::cout << "management_keyset first_page="
              << first.page.items[0].id << ","
              << first.page.items[1].id
              << " next_cursor=" << *first.page.next_cursor
              << " concurrent_new=" << new_id
              << " second_page=" << second.page.items.front().id
              << " disabled_source=" << disabled.page.items.front().id
              << " qc_order=" << rule_ids_[1] << "," << rule_ids_[0]
              << std::endl;
}

TEST_F(ManagementQueryPostgresTest,
       ReturnsStaleSummaryAndPagedEvidenceWithoutWritingRun) {
    PostgresManagementQueryRepository repository{session()};
    ManagementQueryService service{
        repository,
        std::chrono::system_clock::time_point{
            std::chrono::seconds{1787097600}},
        60,
        3600};

    const auto runs = service.list_task_runs(
        {node_code_, task_ids_[2], std::string{"running"},
         {20, std::nullopt}});
    ASSERT_TRUE(runs.status.ok);
    ASSERT_EQ(runs.page.items.size(), 1U);
    EXPECT_TRUE(runs.page.items.front().stale);

    const auto detail = service.find_task_run(node_code_, run_id_);
    ASSERT_TRUE(detail.status.ok);
    ASSERT_TRUE(detail.item.has_value());
    EXPECT_EQ(detail.item->record.raw_file_count, 1);
    EXPECT_EQ(detail.item->record.parsed_record_count, 1);
    EXPECT_EQ(detail.item->record.qc_result_count, 1);
    EXPECT_EQ(detail.item->record.alert_count, 1);

    const auto raw = service.list_raw_files(
        {run_id_, {20, std::nullopt}});
    const auto parsed = service.list_parsed_records(
        {run_id_, {20, std::nullopt}});
    const auto qc = service.list_qc_results(
        {run_id_, std::string{"failed"}, {20, std::nullopt}});
    const auto alerts = service.list_alerts(
        {node_code_, run_id_, std::string{"open"},
         std::string{"failed"}, {20, std::nullopt}});

    ASSERT_EQ(raw.page.items.size(), 1U);
    ASSERT_EQ(parsed.page.items.size(), 1U);
    ASSERT_EQ(qc.page.items.size(), 1U);
    ASSERT_EQ(alerts.page.items.size(), 1U);
    EXPECT_EQ(raw.page.items.front().id, raw_id_);
    EXPECT_EQ(parsed.page.items.front().raw_file_id, raw_id_);
    EXPECT_EQ(qc.page.items.front().parsed_record_id, parsed_id_);
    EXPECT_EQ(qc.page.items.front().task_run_id, run_id_);
    EXPECT_EQ(alerts.page.items.front().task_run_id, run_id_);

    const auto persisted = session().query_one(
        "SELECT status FROM task_runs WHERE id=$1::bigint",
        {run_id_});
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(value_or_empty(*persisted, "status"), "running");
    std::cout << "management_evidence run=" << run_id_
              << " stale=true raw=1 parsed=1 qc=1 alerts=1 stored_status=running"
              << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    const char* connection = std::getenv("LABBRIDGE_DATABASE_URL");
    if (connection == nullptr || std::string{connection}.empty()) {
        std::cout << "LABBRIDGE_DATABASE_URL is not set; skipping "
                     "management query PostgreSQL test\n";
        return 77;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
