cmake_minimum_required(VERSION 3.28)

project(LabBridge
    VERSION 0.1.0
    DESCRIPTION "Private deployment data ingestion and quality control platform"
    LANGUAGES CXX
)

find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)
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

add_library(labbridge_agent_bootstrap
    agent/src/agent_config.cpp
    agent/src/control_plane_client.cpp
    agent/src/startup_handshake.cpp
)

target_link_libraries(labbridge_agent_bootstrap
    PUBLIC
        labbridge_core
        Boost::system
        nlohmann_json::nlohmann_json
        yaml-cpp
)

target_include_directories(labbridge_agent_bootstrap
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
target_link_libraries(labbridge_agent
    PRIVATE
        labbridge_agent_bootstrap
        labbridge_agent_domain
)

if(BUILD_TESTING)
    include(FetchContent)

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)

    function(add_labbridge_test target)
        cmake_parse_arguments(
            ARG
            ""
            "TIMEOUT"
            "SOURCES;LINK_LIBRARIES;LABELS"
            ${ARGN}
        )
        if(NOT ARG_SOURCES)
            message(FATAL_ERROR "add_labbridge_test(${target}) requires SOURCES")
        endif()

        add_executable(${target} ${ARG_SOURCES})
        if(ARG_LINK_LIBRARIES)
            target_link_libraries(${target} PRIVATE ${ARG_LINK_LIBRARIES})
        endif()
        add_test(NAME ${target} COMMAND ${target})

        if(NOT ARG_TIMEOUT)
            set(ARG_TIMEOUT 30)
        endif()
        set_tests_properties(
            ${target}
            PROPERTIES
                LABELS "${ARG_LABELS}"
                TIMEOUT ${ARG_TIMEOUT}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endfunction()

    function(add_labbridge_postgres_test target)
        cmake_parse_arguments(
            ARG
            ""
            "TIMEOUT"
            "SOURCES;LINK_LIBRARIES;LABELS"
            ${ARGN}
        )
        if(NOT ARG_TIMEOUT)
            set(ARG_TIMEOUT 120)
        endif()

        add_labbridge_test(
            ${target}
            SOURCES ${ARG_SOURCES}
            LINK_LIBRARIES ${ARG_LINK_LIBRARIES}
            LABELS postgres ${ARG_LABELS}
            TIMEOUT ${ARG_TIMEOUT}
        )
        set_tests_properties(${target} PROPERTIES SKIP_RETURN_CODE 77)
        set_property(GLOBAL APPEND PROPERTY LABBRIDGE_POSTGRES_TEST_TARGETS ${target})
    endfunction()

    function(add_labbridge_smoke target source)
        cmake_parse_arguments(
            ARG
            "AGENT;POSTGRES;HTTP;REAL_POSTGRES;CONTRACT"
            ""
            ""
            ${ARGN}
        )

        set(libraries labbridge_server_domain)
        if(ARG_AGENT)
            list(APPEND libraries labbridge_agent_domain)
        endif()
        if(ARG_POSTGRES)
            list(APPEND libraries labbridge_server_postgres)
        endif()
        if(ARG_HTTP)
            list(APPEND libraries labbridge_server_http)
        endif()

        set(labels component)
        if(ARG_CONTRACT)
            list(APPEND labels contract)
        endif()

        if(ARG_REAL_POSTGRES)
            add_labbridge_postgres_test(
                ${target}
                SOURCES ${source}
                LINK_LIBRARIES ${libraries}
            )
        else()
            add_labbridge_test(
                ${target}
                SOURCES ${source}
                LINK_LIBRARIES ${libraries}
                LABELS ${labels}
            )
        endif()
    endfunction()

    add_labbridge_test(
        agent_config_test
        SOURCES tests/unit/agent/agent_config_test.cpp
        LINK_LIBRARIES GTest::gtest_main labbridge_agent_bootstrap
        LABELS unit
        TIMEOUT 10
    )

    add_labbridge_test(
        agent_config_file_test
        SOURCES tests/component/agent/agent_config_file_test.cpp
        LINK_LIBRARIES GTest::gtest_main labbridge_agent_bootstrap
        LABELS component
    )

    add_labbridge_test(
        control_plane_client_test
        SOURCES
            tests/component/agent/control_plane_client_test.cpp
            tests/support/agent/mock_http_server.cpp
        LINK_LIBRARIES
            GTest::gtest_main
            labbridge_agent_bootstrap
            Threads::Threads
        LABELS component contract
        TIMEOUT 10
    )
    target_include_directories(
        control_plane_client_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    add_labbridge_test(
        startup_handshake_test
        SOURCES
            tests/component/agent/startup_handshake_test.cpp
            tests/support/agent/mock_http_server.cpp
        LINK_LIBRARIES
            GTest::gtest_main
            labbridge_agent_bootstrap
            Threads::Threads
        LABELS component
        TIMEOUT 10
    )
    target_include_directories(
        startup_handshake_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    add_labbridge_postgres_test(
        node_lifecycle_postgres_test
        SOURCES tests/integration/postgres/node_lifecycle_postgres_test.cpp
        LINK_LIBRARIES GTest::gtest labbridge_server_postgres
    )

    add_labbridge_smoke(labbridge_phase1_smoke tests/integration/phase1_smoke.cpp AGENT)
    add_labbridge_smoke(labbridge_phase2_postgres_repository_smoke tests/integration/phase2_postgres_repository_smoke.cpp POSTGRES)
    add_labbridge_smoke(labbridge_phase3_libpq_session_smoke tests/integration/phase3_libpq_session_smoke.cpp POSTGRES)
    add_labbridge_smoke(labbridge_phase5_config_repository_smoke tests/integration/phase5_config_repository_smoke.cpp POSTGRES)
    add_labbridge_smoke(labbridge_phase6_task_run_repository_smoke tests/integration/phase6_task_run_repository_smoke.cpp POSTGRES)
    add_labbridge_smoke(labbridge_phase7_real_postgres_control_flow_smoke tests/integration/phase7_real_postgres_control_flow_smoke.cpp POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase8_result_repository_smoke tests/integration/phase8_result_repository_smoke.cpp POSTGRES)
    add_labbridge_smoke(labbridge_phase9_real_postgres_result_flow_smoke tests/integration/phase9_real_postgres_result_flow_smoke.cpp POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase10_qc_repository_smoke tests/integration/phase10_qc_repository_smoke.cpp POSTGRES)
    add_labbridge_smoke(labbridge_phase11_real_postgres_qc_flow_smoke tests/integration/phase11_real_postgres_qc_flow_smoke.cpp POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase13_alert_repository_smoke tests/integration/phase13_alert_repository_smoke.cpp POSTGRES)
    add_labbridge_smoke(labbridge_phase13_real_postgres_alert_flow_smoke tests/integration/phase13_real_postgres_alert_flow_smoke.cpp POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase14_control_plane_query_smoke tests/integration/phase14_control_plane_query_smoke.cpp)
    add_labbridge_smoke(labbridge_phase14_real_postgres_query_flow_smoke tests/integration/phase14_real_postgres_query_flow_smoke.cpp POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase15_control_plane_archive_trace_smoke tests/integration/phase15_control_plane_archive_trace_smoke.cpp)
    add_labbridge_smoke(labbridge_phase15_real_postgres_archive_trace_smoke tests/integration/phase15_real_postgres_archive_trace_smoke.cpp POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase16_agent_report_service_smoke tests/integration/phase16_agent_report_service_smoke.cpp)
    add_labbridge_smoke(labbridge_phase16_real_postgres_agent_report_service_smoke tests/integration/phase16_real_postgres_agent_report_service_smoke.cpp POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase17_http_agent_report_smoke tests/integration/phase17_http_agent_report_smoke.cpp HTTP CONTRACT)
    add_labbridge_smoke(labbridge_phase17_real_postgres_http_agent_report_smoke tests/integration/phase17_real_postgres_http_agent_report_smoke.cpp HTTP POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase18_agent_report_reliability_smoke tests/integration/phase18_agent_report_reliability_smoke.cpp POSTGRES)
    add_labbridge_smoke(labbridge_phase18_real_postgres_agent_report_reliability_smoke tests/integration/phase18_real_postgres_agent_report_reliability_smoke.cpp POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_phase19_http_agent_control_smoke tests/integration/phase19_http_agent_control_smoke.cpp HTTP CONTRACT)
    add_labbridge_smoke(labbridge_phase19_real_postgres_http_agent_control_smoke tests/integration/phase19_real_postgres_http_agent_control_smoke.cpp HTTP POSTGRES REAL_POSTGRES)
    add_labbridge_smoke(labbridge_code_quality_smoke tests/integration/phase16_review_code_quality_smoke.cpp AGENT)

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
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunPostgresTests.cmake
        DEPENDS ${labbridge_postgres_test_targets}
        USES_TERMINAL
    )
endif()
