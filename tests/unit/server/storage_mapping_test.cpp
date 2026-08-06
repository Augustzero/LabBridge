#include "labbridge/server/postgres/storage_mapping.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

TEST(StorageMappingTest, MapsAllSupportedBooleanRepresentations) {
    EXPECT_TRUE(labbridge::server::storage::bool_value("true"));
    EXPECT_TRUE(labbridge::server::storage::bool_value("t"));
    EXPECT_TRUE(labbridge::server::storage::bool_value("1"));
    EXPECT_FALSE(labbridge::server::storage::bool_value("false"));
    EXPECT_FALSE(labbridge::server::storage::bool_value("f"));
    EXPECT_FALSE(labbridge::server::storage::bool_value("0"));
}

TEST(StorageMappingTest, RejectsUnknownBooleanRepresentation) {
    EXPECT_THROW(
        static_cast<void>(labbridge::server::storage::bool_value("yes")),
        std::runtime_error);
    EXPECT_THROW(
        static_cast<void>(labbridge::server::storage::bool_value("")),
        std::runtime_error);
}

TEST(StorageMappingTest, RoundTripsSupportedNodeStatuses) {
    for (const auto value : {
             labbridge::core::NodeStatus::Online,
             labbridge::core::NodeStatus::Offline}) {
        EXPECT_EQ(
            labbridge::server::storage::node_status_from_storage(
                labbridge::server::storage::to_storage(value)),
            value);
    }
}

TEST(StorageMappingTest, RejectsUnknownNodeStatusValues) {
    EXPECT_THROW(
        static_cast<void>(labbridge::server::storage::to_storage(
            static_cast<labbridge::core::NodeStatus>(99))),
        std::runtime_error);
    EXPECT_THROW(
        static_cast<void>(
            labbridge::server::storage::node_status_from_storage("unknown")),
        std::runtime_error);
}

TEST(StorageMappingTest, RoundTripsSupportedTaskRunStatuses) {
    for (const auto value : {
             labbridge::core::TaskRunStatus::Pending,
             labbridge::core::TaskRunStatus::Running,
             labbridge::core::TaskRunStatus::Succeeded,
             labbridge::core::TaskRunStatus::Failed}) {
        EXPECT_EQ(
            labbridge::server::storage::task_run_status_from_storage(
                labbridge::server::storage::to_storage(value)),
            value);
    }
}

TEST(StorageMappingTest, RejectsUnknownTaskRunStatusValues) {
    EXPECT_THROW(
        static_cast<void>(labbridge::server::storage::to_storage(
            static_cast<labbridge::core::TaskRunStatus>(99))),
        std::runtime_error);
    EXPECT_THROW(
        static_cast<void>(
            labbridge::server::storage::task_run_status_from_storage(
                "unknown")),
        std::runtime_error);
}

TEST(StorageMappingTest, RoundTripsSupportedSourceTypes) {
    for (const auto value : {
             labbridge::core::SourceType::LocalDirectory,
             labbridge::core::SourceType::Ftp,
             labbridge::core::SourceType::Oracle}) {
        EXPECT_EQ(
            labbridge::server::storage::source_type_from_storage(
                labbridge::server::storage::to_storage(value)),
            value);
    }
}

TEST(StorageMappingTest, RejectsUnknownSourceTypeValues) {
    EXPECT_THROW(
        static_cast<void>(labbridge::server::storage::to_storage(
            static_cast<labbridge::core::SourceType>(99))),
        std::runtime_error);
    EXPECT_THROW(
        static_cast<void>(
            labbridge::server::storage::source_type_from_storage("unknown")),
        std::runtime_error);
}

}  // namespace
