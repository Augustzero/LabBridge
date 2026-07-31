if("$ENV{LABBRIDGE_DATABASE_URL}" STREQUAL "")
    message(FATAL_ERROR
        "LABBRIDGE_DATABASE_URL is required for the PostgreSQL test suite")
endif()

execute_process(
    COMMAND
        "${CTEST_COMMAND}"
        --test-dir "${TEST_BINARY_DIR}"
        -L postgres
        --output-on-failure
    RESULT_VARIABLE test_result
)

if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "PostgreSQL test suite failed with exit code ${test_result}")
endif()
