cmake_minimum_required(VERSION 3.28)

project(LabBridge
    VERSION 0.1.0
    DESCRIPTION "Private deployment data ingestion and quality control platform"
    LANGUAGES CXX
)

find_package(PostgreSQL REQUIRED)

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

add_executable(labbridge_server
    server/src/config_repository.cpp
    server/src/config_service.cpp
    server/src/libpq_sql_session.cpp
    server/src/main.cpp
    server/src/node_repository.cpp
    server/src/postgres_config_repository.cpp
    server/src/postgres_node_repository.cpp
    server/src/postgres_qc_repository.cpp
    server/src/postgres_result_repository.cpp
    server/src/postgres_task_run_repository.cpp
    server/src/node_service.cpp
    server/src/qc_repository.cpp
    server/src/qc_service.cpp
    server/src/result_repository.cpp
    server/src/result_service.cpp
    server/src/task_run_repository.cpp
    server/src/task_run_service.cpp
)

target_link_libraries(labbridge_server
    PRIVATE
        labbridge_core
        PostgreSQL::PostgreSQL
)

target_include_directories(labbridge_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_agent
    agent/src/main.cpp
    agent/src/local_dir_collector.cpp
    agent/src/csv_parser.cpp
    agent/src/basic_qc_rules.cpp
)

target_link_libraries(labbridge_agent
    PRIVATE
        labbridge_core
)

target_include_directories(labbridge_agent
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/agent/include
)

add_executable(labbridge_phase1_smoke
    tests/integration/phase1_smoke.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
    agent/src/local_dir_collector.cpp
    agent/src/csv_parser.cpp
    agent/src/basic_qc_rules.cpp
)

target_link_libraries(labbridge_phase1_smoke
    PRIVATE
        labbridge_core
)

target_include_directories(labbridge_phase1_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
        ${CMAKE_CURRENT_SOURCE_DIR}/agent/include
)

add_executable(labbridge_phase2_postgres_repository_smoke
    tests/integration/phase2_postgres_repository_smoke.cpp
    server/src/postgres_node_repository.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
)

target_link_libraries(labbridge_phase2_postgres_repository_smoke
    PRIVATE
        labbridge_core
)

target_include_directories(labbridge_phase2_postgres_repository_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase3_libpq_session_smoke
    tests/integration/phase3_libpq_session_smoke.cpp
    server/src/libpq_sql_session.cpp
)

target_link_libraries(labbridge_phase3_libpq_session_smoke
    PRIVATE
        labbridge_core
        PostgreSQL::PostgreSQL
)

target_include_directories(labbridge_phase3_libpq_session_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase4_real_postgres_node_flow_smoke
    tests/integration/phase4_real_postgres_node_flow_smoke.cpp
    server/src/libpq_sql_session.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
    server/src/postgres_node_repository.cpp
)

target_link_libraries(labbridge_phase4_real_postgres_node_flow_smoke
    PRIVATE
        labbridge_core
        PostgreSQL::PostgreSQL
)

target_include_directories(labbridge_phase4_real_postgres_node_flow_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase5_config_repository_smoke
    tests/integration/phase5_config_repository_smoke.cpp
    server/src/config_repository.cpp
    server/src/config_service.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
    server/src/postgres_config_repository.cpp
    server/src/postgres_node_repository.cpp
)

target_link_libraries(labbridge_phase5_config_repository_smoke
    PRIVATE
        labbridge_core
)

target_include_directories(labbridge_phase5_config_repository_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase6_task_run_repository_smoke
    tests/integration/phase6_task_run_repository_smoke.cpp
    server/src/config_repository.cpp
    server/src/config_service.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
    server/src/postgres_config_repository.cpp
    server/src/postgres_node_repository.cpp
    server/src/postgres_task_run_repository.cpp
    server/src/task_run_repository.cpp
    server/src/task_run_service.cpp
)

target_link_libraries(labbridge_phase6_task_run_repository_smoke
    PRIVATE
        labbridge_core
)

target_include_directories(labbridge_phase6_task_run_repository_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase7_real_postgres_control_flow_smoke
    tests/integration/phase7_real_postgres_control_flow_smoke.cpp
    server/src/config_repository.cpp
    server/src/config_service.cpp
    server/src/libpq_sql_session.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
    server/src/postgres_config_repository.cpp
    server/src/postgres_node_repository.cpp
    server/src/postgres_task_run_repository.cpp
    server/src/task_run_repository.cpp
    server/src/task_run_service.cpp
)

target_link_libraries(labbridge_phase7_real_postgres_control_flow_smoke
    PRIVATE
        labbridge_core
        PostgreSQL::PostgreSQL
)

target_include_directories(labbridge_phase7_real_postgres_control_flow_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase8_result_repository_smoke
    tests/integration/phase8_result_repository_smoke.cpp
    server/src/postgres_result_repository.cpp
    server/src/result_repository.cpp
    server/src/result_service.cpp
    server/src/task_run_repository.cpp
)

target_link_libraries(labbridge_phase8_result_repository_smoke
    PRIVATE
        labbridge_core
)

target_include_directories(labbridge_phase8_result_repository_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase9_real_postgres_result_flow_smoke
    tests/integration/phase9_real_postgres_result_flow_smoke.cpp
    server/src/config_repository.cpp
    server/src/config_service.cpp
    server/src/libpq_sql_session.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
    server/src/postgres_config_repository.cpp
    server/src/postgres_node_repository.cpp
    server/src/postgres_result_repository.cpp
    server/src/postgres_task_run_repository.cpp
    server/src/result_repository.cpp
    server/src/result_service.cpp
    server/src/task_run_repository.cpp
    server/src/task_run_service.cpp
)

target_link_libraries(labbridge_phase9_real_postgres_result_flow_smoke
    PRIVATE
        labbridge_core
        PostgreSQL::PostgreSQL
)

target_include_directories(labbridge_phase9_real_postgres_result_flow_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase10_qc_repository_smoke
    tests/integration/phase10_qc_repository_smoke.cpp
    server/src/postgres_qc_repository.cpp
    server/src/postgres_result_repository.cpp
    server/src/qc_repository.cpp
    server/src/qc_service.cpp
    server/src/result_repository.cpp
    server/src/result_service.cpp
    server/src/task_run_repository.cpp
)

target_link_libraries(labbridge_phase10_qc_repository_smoke
    PRIVATE
        labbridge_core
)

target_include_directories(labbridge_phase10_qc_repository_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)

add_executable(labbridge_phase11_real_postgres_qc_flow_smoke
    tests/integration/phase11_real_postgres_qc_flow_smoke.cpp
    server/src/config_repository.cpp
    server/src/config_service.cpp
    server/src/libpq_sql_session.cpp
    server/src/node_repository.cpp
    server/src/node_service.cpp
    server/src/postgres_config_repository.cpp
    server/src/postgres_node_repository.cpp
    server/src/postgres_qc_repository.cpp
    server/src/postgres_result_repository.cpp
    server/src/postgres_task_run_repository.cpp
    server/src/qc_repository.cpp
    server/src/qc_service.cpp
    server/src/result_repository.cpp
    server/src/result_service.cpp
    server/src/task_run_repository.cpp
    server/src/task_run_service.cpp
)

target_link_libraries(labbridge_phase11_real_postgres_qc_flow_smoke
    PRIVATE
        labbridge_core
        PostgreSQL::PostgreSQL
)

target_include_directories(labbridge_phase11_real_postgres_qc_flow_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/server/include
)
