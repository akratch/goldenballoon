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

execute_process(
    COMMAND "${MDKR_EXECUTABLE}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_stdout
    ERROR_VARIABLE help_stderr)

if(NOT help_result EQUAL 0)
    message(FATAL_ERROR "--help exited ${help_result}")
endif()
if(NOT help_stderr STREQUAL "")
    message(FATAL_ERROR "--help wrote unexpected stderr: '${help_stderr}'")
endif()

set(restored_help
    "--restored                 original art direction at modern fidelity\n                             (default)")
set(remastered_help
    "--remastered               opt-in work-in-progress art-directed effects")
string(FIND "${help_stdout}" "${restored_help}" restored_help_at)
string(FIND "${help_stdout}" "${remastered_help}" remastered_help_at)
string(FIND "${help_stdout}" "full remaster (default)" stale_help_at)
if(restored_help_at EQUAL -1)
    message(FATAL_ERROR "--help does not identify Restored as the default:\n${help_stdout}")
endif()
if(remastered_help_at EQUAL -1)
    message(FATAL_ERROR "--help does not identify Remastered as opt-in/WIP:\n${help_stdout}")
endif()
if(NOT stale_help_at EQUAL -1)
    message(FATAL_ERROR "--help still calls Remastered the default:\n${help_stdout}")
endif()
