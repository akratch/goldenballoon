if(NOT DEFINED MDKR_EXECUTABLE OR NOT DEFINED MDKR_EXPECTED_VERSION)
    message(FATAL_ERROR "MDKR_EXECUTABLE and MDKR_EXPECTED_VERSION are required")
endif()

execute_process(
    COMMAND "${MDKR_EXECUTABLE}" --version
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)

set(expected "mdkr64 ${MDKR_EXPECTED_VERSION}\n")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "--version exited ${result}")
endif()
if(NOT stdout STREQUAL expected)
    message(FATAL_ERROR "--version stdout was '${stdout}', expected '${expected}'")
endif()
if(NOT stderr STREQUAL "")
    message(FATAL_ERROR "--version wrote unexpected stderr: '${stderr}'")
endif()
