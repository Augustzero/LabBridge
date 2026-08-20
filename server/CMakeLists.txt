add_library(labbridge_server_domain
    src/application/agent_control_service.cpp
    src/application/agent_report_service.cpp
    src/application/alert_service.cpp
    src/application/config_service.cpp
    src/application/management_query_service.cpp
    src/application/node_service.cpp
    src/application/qc_service.cpp
    src/application/query_service.cpp
    src/application/result_service.cpp
    src/application/task_run_service.cpp
)

target_link_libraries(labbridge_server_domain
    PUBLIC
        labbridge_core
    PRIVATE
        OpenSSL::Crypto
)

target_include_directories(labbridge_server_domain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(labbridge_server_postgres
    src/postgres/agent_control_executor.cpp
    src/postgres/agent_report_executor.cpp
    src/postgres/agent_report_receipt_repository.cpp
    src/postgres/alert_repository.cpp
    src/postgres/config_repository.cpp
    src/postgres/libpq_sql_session.cpp
    src/postgres/management_query_repository.cpp
    src/postgres/node_repository.cpp
    src/postgres/qc_repository.cpp
    src/postgres/result_repository.cpp
    src/postgres/sql_transaction.cpp
    src/postgres/task_run_executor.cpp
    src/postgres/task_run_repository.cpp
)

target_link_libraries(labbridge_server_postgres
    PUBLIC
        labbridge_server_domain
        PostgreSQL::PostgreSQL
)

add_library(labbridge_server_http
    src/http/agent_control_http_controller.cpp
    src/http/agent_report_http_controller.cpp
    src/http/http_json_response.cpp
    src/http/task_run_http_controller.cpp
)

target_link_libraries(labbridge_server_http
    PUBLIC
        labbridge_server_domain
        Drogon::Drogon
)

add_executable(labbridge_server src/main.cpp)
target_link_libraries(
    labbridge_server
    PRIVATE
        labbridge_server_http
        labbridge_server_postgres
)
set_target_properties(
    labbridge_server
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
