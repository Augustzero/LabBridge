#include "labbridge/agent/execution/execution_request_codec.h"

#include <gtest/gtest.h>

namespace {

labbridge::core::TaskConfig task_config() {
    return {
        "42",
        "node-1",
        "7",
        "CSV import",
        "local_file_import",
        "* * * * *",
        "csv_observation",
        "basic",
        true,
        {
            "7",
            "node-1",
            labbridge::core::SourceType::LocalDirectory,
            "Inbox",
            R"({"root_path":"/inbox"})",
        },
        {
            {"9", "required_fields", "Required", R"({})"},
        },
    };
}

labbridge::agent::StartTaskRunRequest start_request() {
    return {
        "node-1",
        "42",
        "execution-1",
        "2026-08-12T00:00:00Z",
        "2026-08-12T00:00:01Z",
        "scheduled",
    };
}

labbridge::agent::RawFileManifestRequest manifest_request() {
    return {
        "100",
        "node-1",
        "manifest-1",
        {
            {
                "sample.csv",
                "hash",
                "/archive/sample.csv",
                42,
                "2026-08-12T00:00:00Z",
                "archived_local",
            },
        },
    };
}

labbridge::agent::TaskRunReportRequest report_request() {
    labbridge::agent::TaskRunReportRequest request;
    request.task_run_id = "100";
    request.node_code = "node-1";
    request.idempotency_key = "report-1";
    request.finished_at = "2026-08-12T00:00:02Z";
    request.items_total = 1;
    request.items_success = 1;
    request.parsed_records.push_back({
        "200",
        {
            "ST01",
            "DV01",
            "2026-08-12T00:00:00Z",
            R"({"value":42})",
        },
        "parsed",
        {
            {"9", "error", "passed", "ok"},
        },
    });
    return request;
}

}  // namespace

TEST(ExecutionRequestCodecTest, RoundTripsCanonicalPayloads) {
    const auto task_json =
        labbridge::agent::encode_task_config(task_config());
    EXPECT_EQ(
        task_json,
        labbridge::agent::encode_task_config(
            labbridge::agent::decode_task_config(task_json)));

    const auto start_json =
        labbridge::agent::encode_start_task_run_request(start_request());
    EXPECT_EQ(
        start_json,
        labbridge::agent::encode_start_task_run_request(
            labbridge::agent::decode_start_task_run_request(start_json)));

    const auto manifest_json =
        labbridge::agent::encode_raw_file_manifest_request(manifest_request());
    EXPECT_EQ(
        manifest_json,
        labbridge::agent::encode_raw_file_manifest_request(
            labbridge::agent::decode_raw_file_manifest_request(
                manifest_json)));

    const auto report_json =
        labbridge::agent::encode_task_run_report_request(report_request());
    EXPECT_EQ(
        report_json,
        labbridge::agent::encode_task_run_report_request(
            labbridge::agent::decode_task_run_report_request(report_json)));
}

TEST(ExecutionRequestCodecTest,
     RejectsUnknownVersionAndInvalidTerminalStatus) {
    EXPECT_THROW(
        labbridge::agent::decode_start_task_run_request(
            R"({"codec_version":2})"),
        labbridge::agent::ExecutionCodecError);

    EXPECT_THROW(
        labbridge::agent::decode_task_run_report_request(R"({
            "codec_version": 1,
            "task_run_id": "1",
            "node_code": "n",
            "idempotency_key": "k",
            "status": "running",
            "finished_at": "t",
            "items_total": 0,
            "items_success": 0,
            "items_failed": 0,
            "error_summary": "",
            "parsed_records": []
        })"),
        labbridge::agent::ExecutionCodecError);
}
