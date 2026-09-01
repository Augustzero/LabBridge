cmake_minimum_required(VERSION 3.22)

project(LabBridge
    VERSION 0.1.0
    DESCRIPTION "Private deployment data ingestion and quality control platform"
    LANGUAGES CXX
)

option(LABBRIDGE_BUILD_SERVER "Build the LabBridge server" ON)
option(LABBRIDGE_BUILD_AGENT "Build the LabBridge edge agent" ON)
option(BUILD_TESTING "Build LabBridge tests" OFF)
include(CTest)

if(NOT LABBRIDGE_BUILD_SERVER AND NOT LABBRIDGE_BUILD_AGENT)
    message(FATAL_ERROR
        "At least one of LABBRIDGE_BUILD_SERVER or "
        "LABBRIDGE_BUILD_AGENT must be enabled"
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json REQUIRED)

if(LABBRIDGE_BUILD_AGENT)
    find_package(SQLite3 REQUIRED)
    find_package(Boost REQUIRED COMPONENTS system)
    find_package(yaml-cpp REQUIRED)
endif()

if(LABBRIDGE_BUILD_SERVER)
    find_package(PostgreSQL REQUIRED)
    cmake_policy(PUSH)
    if(POLICY CMP0153)
        # Ubuntu 的 Drogon 1.8.7 配置仍调用已废弃的 exec_program。
        cmake_policy(SET CMP0153 OLD)
    endif()
    find_package(Drogon REQUIRED)
    cmake_policy(POP)
endif()

if(LABBRIDGE_BUILD_AGENT OR BUILD_TESTING)
    find_package(Threads REQUIRED)
endif()

add_subdirectory(core)

if(LABBRIDGE_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(LABBRIDGE_BUILD_AGENT)
    add_subdirectory(agent)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
