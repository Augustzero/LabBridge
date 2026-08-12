cmake_minimum_required(VERSION 3.28)

project(LabBridge
    VERSION 0.1.0
    DESCRIPTION "Private deployment data ingestion and quality control platform"
    LANGUAGES CXX
)

find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(SQLite3 REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)
cmake_policy(PUSH)
cmake_policy(SET CMP0153 OLD)
find_package(Drogon REQUIRED)
cmake_policy(POP)
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(core)
add_subdirectory(server)
add_subdirectory(agent)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()