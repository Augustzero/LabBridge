#include "labbridge/server/http/task_run_http_controller.h"
#include "support/server/in_memory_repositories.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

drogon::HttpResponsePtr invoke(
    const labbridge::server::TaskRunHttpController& controller,
    const Json::Value& body,
    bool json_content_type = true) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    if (json_content_type) {
        request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    request->setBody(body.toStyledString());

    drogon::HttpResponsePtr response;
    controller.post_start(
        request,
        [&response](const drogon::HttpResponsePtr& current) {
            response = current;
        });
    EXPECT_NE(response, nullptr);
    return response;
}

const Json::Value& response_json(const drogon::HttpResponsePtr& response) {
    const auto& json = response->getJsonObject();
    EXPECT_NE(json, nullptr);
    return *json;
}

Json::Value valid_body(const std::string& task_id) {
    Json::Value body;
    body["node_code"] = "node-http";
    body["task_id"] = task_id;
    body["execution_key"] = "http-scheduled-key";
    body["scheduled_for"] = "2026-08-08T10:00:00Z";
    body["started_at"] = "2026-08-08T10:00:01Z";
    body["trigger_type"] = "scheduled";
    return body;
}

class TaskRunHttpControllerTest : public testing::Test {
protected:
    void SetUp() override {
        labbridge::server::TaskRecord task;
        task.node_code = "node-http";
        task.data_source_id = "source-http";
        task.name = "HTTP scheduled task";
        task.task_type = "local_file_import";
        task.schedule_expr = "*/5 * * * *";
        task.parser_type = "csv_observation";
        task.enabled = true;
        task_id_ = configs_.create_task(std::move(task));
        controller_ =
            std::make_unique<labbridge::server::TaskRunHttpController>(
                [this](const labbridge::server::StartTaskRunRequest& request) {
                    return service_.start(request);
                });
    }

    labbridge::server::InMemoryConfigRepository configs_;
    labbridge::server::InMemoryTaskRunRepository runs_;
    labbridge::server::TaskRunService service_{configs_, runs_};
    std::string task_id_;
    std::unique_ptr<labbridge::server::TaskRunHttpController> controller_;
};

TEST_F(TaskRunHttpControllerTest, CreatesAndReplaysScheduledRun) {
    const auto first = invoke(*controller_, valid_body(task_id_));
    const auto second = invoke(*controller_, valid_body(task_id_));

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->statusCode(), drogon::k201Created);
    EXPECT_EQ(second->statusCode(), drogon::k201Created);
    const auto& first_json = response_json(first);
    const auto& second_json = response_json(second);
    EXPECT_TRUE(first_json["ok"].asBool());
    EXPECT_FALSE(first_json["data"]["replayed"].asBool());
    EXPECT_TRUE(second_json["data"]["replayed"].asBool());
    EXPECT_EQ(
        second_json["data"]["task_run_id"].asString(),
        first_json["data"]["task_run_id"].asString());

    const auto stored = service_.find_run(
        first_json["data"]["task_run_id"].asString());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->execution_key, "http-scheduled-key");
    EXPECT_EQ(stored->scheduled_for, "2026-08-08T10:00:00Z");
    EXPECT_EQ(stored->started_at, "2026-08-08T10:00:01Z");
}

TEST_F(TaskRunHttpControllerTest, ReturnsConflictForChangedScheduledIdentity) {
    ASSERT_EQ(
        invoke(*controller_, valid_body(task_id_))->statusCode(),
        drogon::k201Created);
    auto changed = valid_body(task_id_);
    changed["scheduled_for"] = "2026-08-08T10:05:00Z";

    const auto response = invoke(*controller_, changed);

    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->statusCode(), drogon::k409Conflict);
    EXPECT_EQ(response_json(response)["error"]["code"].asString(), "conflict");
}

TEST_F(TaskRunHttpControllerTest, RejectsInvalidRequestAtHttpBoundary) {
    auto missing = valid_body(task_id_);
    missing.removeMember("execution_key");
    const auto missing_response = invoke(*controller_, missing);

    auto invalid_time = valid_body(task_id_);
    invalid_time["scheduled_for"] = "2026-02-29T10:00:00Z";
    const auto invalid_time_response = invoke(*controller_, invalid_time);
    const auto content_type_response =
        invoke(*controller_, valid_body(task_id_), false);

    ASSERT_NE(missing_response, nullptr);
    ASSERT_NE(invalid_time_response, nullptr);
    ASSERT_NE(content_type_response, nullptr);
    EXPECT_EQ(missing_response->statusCode(), drogon::k400BadRequest);
    EXPECT_EQ(invalid_time_response->statusCode(), drogon::k400BadRequest);
    EXPECT_EQ(
        response_json(missing_response)["error"]["code"].asString(),
        "invalid_argument");
    EXPECT_EQ(content_type_response->statusCode(), drogon::k415UnsupportedMediaType);
}

}  // namespace
