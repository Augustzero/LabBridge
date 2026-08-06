include(FetchContent)
include(${PROJECT_SOURCE_DIR}/cmake/LabBridgeTesting.cmake)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

add_library(labbridge_agent_test_support
    support/agent/mock_http_server.cpp
)
target_include_directories(
    labbridge_agent_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(
    labbridge_agent_test_support
    PUBLIC
        Boost::system
        Threads::Threads
)

add_library(labbridge_server_test_support
    support/server/in_memory_agent_report_receipt_repository.cpp
    support/server/in_memory_alert_repository.cpp
    support/server/in_memory_config_repository.cpp
    support/server/in_memory_node_repository.cpp
    support/server/in_memory_qc_repository.cpp
    support/server/in_memory_result_repository.cpp
    support/server/in_memory_task_run_repository.cpp
)
target_include_directories(
    labbridge_server_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(
    labbridge_server_test_support
    PUBLIC labbridge_server_domain
)

add_labbridge_test(
    agent_config_test
    SOURCES unit/agent/agent_config_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_agent_bootstrap
    LABELS unit
    TIMEOUT 10
)

add_labbridge_test(
    basic_qc_rules_test
    SOURCES unit/agent/basic_qc_rules_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_agent_domain
    LABELS unit
    TIMEOUT 10
)

add_labbridge_test(
    csv_parser_test
    SOURCES component/agent/csv_parser_test.cpp
    LINK_LIBRARIES
        GTest::gtest_main
        labbridge_agent_domain
        nlohmann_json::nlohmann_json
    LABELS component
)

add_labbridge_test(
    local_dir_collector_test
    SOURCES component/agent/local_dir_collector_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_agent_domain
    LABELS component
)

add_labbridge_test(
    agent_config_file_test
    SOURCES component/agent/agent_config_file_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_agent_bootstrap
    LABELS component
)

add_labbridge_test(
    control_plane_client_test
    SOURCES component/agent/control_plane_client_test.cpp
    LINK_LIBRARIES
        GTest::gtest_main
        labbridge_agent_bootstrap
        labbridge_agent_test_support
    LABELS component contract
    TIMEOUT 10
)

add_labbridge_test(
    startup_handshake_test
    SOURCES component/agent/startup_handshake_test.cpp
    LINK_LIBRARIES
        GTest::gtest_main
        labbridge_agent_bootstrap
        labbridge_agent_test_support
    LABELS component
    TIMEOUT 10
)

add_labbridge_test(
    storage_mapping_test
    SOURCES unit/server/storage_mapping_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_server_postgres
    LABELS unit
    TIMEOUT 10
)

add_labbridge_test(
    sql_transaction_test
    SOURCES unit/server/sql_transaction_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_server_postgres
    LABELS unit
    TIMEOUT 10
)

add_labbridge_test(
    agent_report_receipt_repository_test
    SOURCES unit/server/agent_report_receipt_repository_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_server_postgres
    LABELS unit
    TIMEOUT 10
)

add_labbridge_test(
    node_service_test
    SOURCES component/server/node_service_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_server_test_support
    LABELS component
)

add_labbridge_test(
    config_service_test
    SOURCES component/server/config_service_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_server_test_support
    LABELS component
)

add_labbridge_test(
    task_run_service_test
    SOURCES component/server/task_run_service_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_server_test_support
    LABELS component
)

add_labbridge_test(
    alert_service_test
    SOURCES component/server/alert_service_test.cpp
    LINK_LIBRARIES GTest::gtest_main labbridge_server_test_support
    LABELS component
)

add_labbridge_postgres_test(
    node_lifecycle_postgres_test
    SOURCES integration/postgres/node_lifecycle_postgres_test.cpp
    LINK_LIBRARIES GTest::gtest labbridge_server_postgres
)

add_labbridge_smoke(
    labbridge_phase1_smoke
    component/agent/phase1_smoke.cpp
    AGENT SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase2_postgres_repository_smoke
    component/server/phase2_postgres_repository_smoke.cpp
    POSTGRES
)
add_labbridge_smoke(
    labbridge_phase3_libpq_session_smoke
    component/server/phase3_libpq_session_smoke.cpp
    POSTGRES
)
add_labbridge_smoke(
    labbridge_phase5_config_repository_smoke
    component/server/phase5_config_repository_smoke.cpp
    POSTGRES
)
add_labbridge_smoke(
    labbridge_phase6_task_run_repository_smoke
    component/server/phase6_task_run_repository_smoke.cpp
    POSTGRES
)
add_labbridge_smoke(
    labbridge_phase7_real_postgres_control_flow_smoke
    integration/postgres/phase7_real_postgres_control_flow_smoke.cpp
    POSTGRES REAL_POSTGRES
)
add_labbridge_smoke(
    labbridge_phase8_result_repository_smoke
    component/server/phase8_result_repository_smoke.cpp
    POSTGRES SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase9_real_postgres_result_flow_smoke
    integration/postgres/phase9_real_postgres_result_flow_smoke.cpp
    POSTGRES REAL_POSTGRES
)
add_labbridge_smoke(
    labbridge_phase10_qc_repository_smoke
    component/server/phase10_qc_repository_smoke.cpp
    POSTGRES SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase11_real_postgres_qc_flow_smoke
    integration/postgres/phase11_real_postgres_qc_flow_smoke.cpp
    POSTGRES REAL_POSTGRES
)
add_labbridge_smoke(
    labbridge_phase13_alert_repository_smoke
    component/server/phase13_alert_repository_smoke.cpp
    POSTGRES SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase13_real_postgres_alert_flow_smoke
    integration/postgres/phase13_real_postgres_alert_flow_smoke.cpp
    POSTGRES REAL_POSTGRES
)
add_labbridge_smoke(
    labbridge_phase14_control_plane_query_smoke
    component/server/phase14_control_plane_query_smoke.cpp
    SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase14_real_postgres_query_flow_smoke
    integration/postgres/phase14_real_postgres_query_flow_smoke.cpp
    POSTGRES REAL_POSTGRES
)
add_labbridge_smoke(
    labbridge_phase15_control_plane_archive_trace_smoke
    component/server/phase15_control_plane_archive_trace_smoke.cpp
    SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase15_real_postgres_archive_trace_smoke
    integration/postgres/phase15_real_postgres_archive_trace_smoke.cpp
    POSTGRES REAL_POSTGRES
)
add_labbridge_smoke(
    labbridge_phase16_agent_report_service_smoke
    component/server/phase16_agent_report_service_smoke.cpp
    GTEST SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase16_real_postgres_agent_report_service_smoke
    integration/postgres/phase16_real_postgres_agent_report_service_smoke.cpp
    POSTGRES REAL_POSTGRES SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase17_http_agent_report_smoke
    component/server/phase17_http_agent_report_smoke.cpp
    HTTP CONTRACT SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase17_real_postgres_http_agent_report_smoke
    integration/postgres/phase17_real_postgres_http_agent_report_smoke.cpp
    HTTP POSTGRES REAL_POSTGRES
)
add_labbridge_smoke(
    labbridge_phase18_agent_report_reliability_smoke
    component/server/phase18_agent_report_reliability_smoke.cpp
    GTEST POSTGRES SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase18_real_postgres_agent_report_reliability_smoke
    integration/postgres/phase18_real_postgres_agent_report_reliability_smoke.cpp
    POSTGRES REAL_POSTGRES
)
add_labbridge_smoke(
    labbridge_phase19_http_agent_control_smoke
    component/server/phase19_http_agent_control_smoke.cpp
    HTTP CONTRACT SERVER_SUPPORT
)
add_labbridge_smoke(
    labbridge_phase19_real_postgres_http_agent_control_smoke
    integration/postgres/phase19_real_postgres_http_agent_control_smoke.cpp
    HTTP POSTGRES REAL_POSTGRES
)

get_property(
    labbridge_postgres_test_targets
    GLOBAL
    PROPERTY LABBRIDGE_POSTGRES_TEST_TARGETS
)
add_custom_target(
    labbridge_postgres_tests
    COMMAND
        ${CMAKE_COMMAND}
        -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
        -DTEST_BINARY_DIR=${CMAKE_BINARY_DIR}
        -P ${PROJECT_SOURCE_DIR}/cmake/RunPostgresTests.cmake
    DEPENDS ${labbridge_postgres_test_targets}
    USES_TERMINAL
)
