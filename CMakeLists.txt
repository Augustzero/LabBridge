cmake_minimum_required(VERSION 3.28)

project(LabBridge
    VERSION 0.1.0
    DESCRIPTION "Private deployment data ingestion and quality control platform"
    LANGUAGES CXX
)

find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
cmake_policy(PUSH)
cmake_policy(SET CMP0153 OLD)
find_package(Drogon REQUIRED)
cmake_policy(POP)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(labbridge_core
    core/src/logging.cpp
)

target_include_directories(labbridge_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)

target_compile_features(labbridge_core PUBLIC cxx_std_20)

add_library(labbridge_server_domain
    server/src/agent_control_service.cpp
    server/src/agent_report_receipt_repository.cpp
    server/src/agent_report_service.cpp
    server/src/alert_repository.cpp
    server/src/alert_service.cpp
    server/src/config_repository.cpp
    server/src/config_service.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
    server/src/query_service.cpp
    server/src/qc_repository.cpp
    server/src/qc_service.cpp
    server/src/result_repository.cpp
    server/src/result_service.cpp
    server/src/task_run_repository.cpp
    server/src/task_run_service.cpp
)

target_link_libraries(labbridge_server_domain
    PUBLIC
        labbridge_core
        OpenSSL::Crypto
)

target_include_directories(labbridge_server_domain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_library(labbridge_server_postgres
    server/src/libpq_sql_session.cpp
    server/src/postgres_agent_control_executor.cpp
    server/src/postgres_agent_report_executor.cpp
    server/src/postgres_agent_report_receipt_repository.cpp
    server/src/postgres_alert_repository.cpp
    server/src/postgres_config_repository.cpp
    server/src/postgres_node_repository.cpp
    server/src/postgres_qc_repository.cpp
    server/src/postgres_result_repository.cpp
    server/src/postgres_task_run_repository.cpp
    server/src/sql_transaction.cpp
)

target_link_libraries(labbridge_server_postgres
    PUBLIC
        labbridge_server_domain
        PostgreSQL::PostgreSQL
)

add_library(labbridge_agent_domain
    agent/src/local_dir_collector.cpp
    agent/src/csv_parser.cpp
    agent/src/basic_qc_rules.cpp
)

target_link_libraries(labbridge_agent_domain
    PUBLIC
        labbridge_core
)

target_include_directories(labbridge_agent_domain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/agent/include
)

add_library(labbridge_server_http
    server/src/agent_control_http_controller.cpp
    server/src/agent_report_http_controller.cpp
    server/src/http_json_response.cpp
)

target_link_libraries(labbridge_server_http
    PUBLIC
        labbridge_server_domain
        Drogon::Drogon
)

add_executable(labbridge_server server/src/main.cpp)
target_link_libraries(labbridge_server PRIVATE labbridge_server_http labbridge_server_postgres)

add_executable(labbridge_agent agent/src/main.cpp)
target_link_libraries(labbridge_agent PRIVATE labbridge_agent_domain)

function(add_labbridge_smoke target source)
    cmake_parse_arguments(ARG "AGENT;POSTGRES;HTTP" "" "" ${ARGN})

    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE labbridge_server_domain)

    if(ARG_AGENT)
        target_link_libraries(${target} PRIVATE labbridge_agent_domain)
    endif()
    if(ARG_POSTGRES)
        target_link_libraries(${target} PRIVATE labbridge_server_postgres)
    endif()
    if(ARG_HTTP)
        target_link_libraries(${target} PRIVATE labbridge_server_http)
    endif()
    if(BUILD_TESTING)
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endfunction()

add_labbridge_smoke(labbridge_phase1_smoke tests/integration/phase1_smoke.cpp AGENT)
add_labbridge_smoke(labbridge_phase2_postgres_repository_smoke tests/integration/phase2_postgres_repository_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase3_libpq_session_smoke tests/integration/phase3_libpq_session_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase4_real_postgres_node_flow_smoke tests/integration/phase4_real_postgres_node_flow_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase5_config_repository_smoke tests/integration/phase5_config_repository_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase6_task_run_repository_smoke tests/integration/phase6_task_run_repository_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase7_real_postgres_control_flow_smoke tests/integration/phase7_real_postgres_control_flow_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase8_result_repository_smoke tests/integration/phase8_result_repository_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase9_real_postgres_result_flow_smoke tests/integration/phase9_real_postgres_result_flow_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase10_qc_repository_smoke tests/integration/phase10_qc_repository_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase11_real_postgres_qc_flow_smoke tests/integration/phase11_real_postgres_qc_flow_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase13_alert_repository_smoke tests/integration/phase13_alert_repository_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase13_real_postgres_alert_flow_smoke tests/integration/phase13_real_postgres_alert_flow_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase14_control_plane_query_smoke tests/integration/phase14_control_plane_query_smoke.cpp)
add_labbridge_smoke(labbridge_phase14_real_postgres_query_flow_smoke tests/integration/phase14_real_postgres_query_flow_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase15_control_plane_archive_trace_smoke tests/integration/phase15_control_plane_archive_trace_smoke.cpp)
add_labbridge_smoke(labbridge_phase15_real_postgres_archive_trace_smoke tests/integration/phase15_real_postgres_archive_trace_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase16_agent_report_service_smoke tests/integration/phase16_agent_report_service_smoke.cpp)
add_labbridge_smoke(labbridge_phase16_real_postgres_agent_report_service_smoke tests/integration/phase16_real_postgres_agent_report_service_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase17_http_agent_report_smoke tests/integration/phase17_http_agent_report_smoke.cpp HTTP)
add_labbridge_smoke(labbridge_phase17_real_postgres_http_agent_report_smoke tests/integration/phase17_real_postgres_http_agent_report_smoke.cpp HTTP POSTGRES)
add_labbridge_smoke(labbridge_phase18_agent_report_reliability_smoke tests/integration/phase18_agent_report_reliability_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase18_real_postgres_agent_report_reliability_smoke tests/integration/phase18_real_postgres_agent_report_reliability_smoke.cpp POSTGRES)
add_labbridge_smoke(labbridge_phase19_http_agent_control_smoke tests/integration/phase19_http_agent_control_smoke.cpp HTTP)
add_labbridge_smoke(labbridge_phase19_real_postgres_http_agent_control_smoke tests/integration/phase19_real_postgres_http_agent_control_smoke.cpp HTTP POSTGRES)
add_labbridge_smoke(labbridge_code_quality_smoke tests/integration/phase16_review_code_quality_smoke.cpp AGENT)
