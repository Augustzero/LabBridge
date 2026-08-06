include_guard(GLOBAL)

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
    set_target_properties(
        ${target}
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    set_tests_properties(
        ${target}
        PROPERTIES
            LABELS "${ARG_LABELS}"
            TIMEOUT ${ARG_TIMEOUT}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
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
        "GTEST;AGENT;SERVER_SUPPORT;POSTGRES;HTTP;REAL_POSTGRES;CONTRACT"
        ""
        ""
        ${ARGN}
    )

    set(libraries labbridge_server_domain)
    if(ARG_GTEST)
        list(APPEND libraries GTest::gtest_main)
    endif()
    if(ARG_SERVER_SUPPORT)
        list(APPEND libraries labbridge_server_test_support)
    endif()
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
