#include "labbridge/agent/execution/task_execution_client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace {

bool is_sha256_hex(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(
               value.begin(),
               value.end(),
               [](unsigned char ch) {
                   return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
               });
}

TEST(TaskExecutionClientTest, GeneratesStableSeparatedRequestKeys) {
    const auto start = labbridge::agent::make_scheduled_execution_key(
        "node-a", "42", "2026-08-08T10:00:00Z");
    const auto start_replay = labbridge::agent::make_scheduled_execution_key(
        "node-a", "42", "2026-08-08T10:00:00Z");
    const auto manifest =
        labbridge::agent::make_manifest_idempotency_key("node-a", "99");
    const auto report =
        labbridge::agent::make_report_idempotency_key("node-a", "99");

    EXPECT_TRUE(is_sha256_hex(start));
    EXPECT_TRUE(is_sha256_hex(manifest));
    EXPECT_TRUE(is_sha256_hex(report));
    EXPECT_EQ(
        start,
        "778cef1d32756c7f84f47da2bac45188ba3e647f860372b2106adf7da71ac6ec");
    EXPECT_EQ(
        manifest,
        "74845dbfc4f0f52c83276a7d9121879bb55001f6cec28be72e8ded1b65c2169d");
    EXPECT_EQ(
        report,
        "5ec9370e4c3e35509ce467136e287ac6368f08b1015f3512b80b1513b877abfc");
    EXPECT_EQ(start_replay, start);
    EXPECT_NE(manifest, report);
    EXPECT_NE(
        start,
        labbridge::agent::make_scheduled_execution_key(
            "node-a", "42", "2026-08-08T10:05:00Z"));
}

}  // namespace
