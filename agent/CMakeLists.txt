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

add_library(labbridge_agent_bootstrap
    src/bootstrap/agent_config.cpp
    src/bootstrap/control_plane_client.cpp
    src/bootstrap/startup_handshake.cpp
    src/bootstrap/utc_time.cpp
)

target_link_libraries(labbridge_agent_bootstrap
    PUBLIC
        labbridge_core
    PRIVATE
        Boost::system
        nlohmann_json::nlohmann_json
        yaml-cpp
)

target_include_directories(labbridge_agent_bootstrap
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(labbridge_agent src/main.cpp)
target_link_libraries(labbridge_agent
    PRIVATE
        labbridge_agent_bootstrap
        labbridge_agent_domain
)
set_target_properties(
    labbridge_agent
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
