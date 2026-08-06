add_library(labbridge_core
    src/logging.cpp
)

target_include_directories(labbridge_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(labbridge_core PUBLIC cxx_std_20)
