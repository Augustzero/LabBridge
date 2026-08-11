add_library(labbridge_agent_domain
    src/collectors/local_dir_collector.cpp
    src/parsers/csv_parser.cpp
    src/qc/basic_qc_rules.cpp
)

target_link_libraries(labbridge_agent_domain
    PUBLIC
        labbridge_core
)

target_include_directories(labbridge_agent_domain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(labbridge_agent_execution
    src/execution/task_execution_client.cpp
    src/execution/local_archive_store.cpp
    src/execution/task_executor.cpp
)

target_link_libraries(labbridge_agent_execution
    PUBLIC
        labbridge_core
    PRIVATE
        OpenSSL::Crypto
        labbridge_agent_domain
        nlohmann_json::nlohmann_json
)

target_include_directories(labbridge_agent_execution
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(labbridge_agent_bootstrap
    src/bootstrap/agent_config.cpp
    src/bootstrap/control_plane_client.cpp
    src/bootstrap/process_signal_monitor.cpp
    src/bootstrap/startup_handshake.cpp
    src/bootstrap/utc_time.cpp
)

target_link_libraries(labbridge_agent_bootstrap
    PUBLIC
        labbridge_core
        labbridge_agent_execution
    PRIVATE
        Boost::system
        nlohmann_json::nlohmann_json
        Threads::Threads
        yaml-cpp
)

target_include_directories(labbridge_agent_bootstrap
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(labbridge_agent_runtime
    src/runtime/agent_runtime.cpp
)

target_link_libraries(labbridge_agent_runtime
    PUBLIC
        labbridge_agent_bootstrap
)

target_include_directories(labbridge_agent_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(labbridge_agent_scheduler
    src/scheduler/task_scheduler.cpp
)

target_link_libraries(labbridge_agent_scheduler
    PUBLIC
        labbridge_agent_runtime
        labbridge_core
        Threads::Threads
)

target_include_directories(labbridge_agent_scheduler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(labbridge_agent src/main.cpp)
target_link_libraries(labbridge_agent
    PRIVATE
        labbridge_agent_bootstrap
        labbridge_agent_domain
        labbridge_agent_runtime
)
set_target_properties(
    labbridge_agent
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
