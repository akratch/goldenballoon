if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PATCH_FILE OR NOT DEFINED CLEANUP_PATCH_FILE
   OR NOT DEFINED INIT_PATCH_FILE OR NOT DEFINED STAGES_PATCH_FILE
   OR NOT DEFINED WORK_PATCH_FILE OR NOT DEFINED PREPARED_PATCH_FILE
   OR NOT DEFINED RETIREMENT_PATCH_FILE OR NOT DEFINED PICOHASH_PATCH_FILE)
    message(FATAL_ERROR "SOURCE_DIR and all seven RTC patch paths plus PICOHASH_PATCH_FILE are required")
endif()

# Patch only FetchContent-owned sources. A reverse dry-run recognizes an already
# applied amendment; a forward dry-run rejects partial/unrecognized source
# before writing. No reset or source-overwrite fallback is permitted.
find_program(GIT_EXECUTABLE git REQUIRED)
function(mdkr_apply_checked_patch patch_file)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch_file}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)
    if(already_applied EQUAL 0)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${patch_file}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE CLEANUP_CHECK_RESULT
        ERROR_VARIABLE CLEANUP_CHECK_ERROR)
    if(NOT CLEANUP_CHECK_RESULT EQUAL 0)
        message(FATAL_ERROR "RTC patch does not match source: ${CLEANUP_CHECK_ERROR}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${patch_file}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE CLEANUP_PATCH_RESULT
        ERROR_VARIABLE CLEANUP_PATCH_ERROR)
    if(NOT CLEANUP_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Could not apply RTC patch: ${CLEANUP_PATCH_ERROR}")
    endif()
endfunction()

# Transaction changes overlap the earlier cleanup constructor hunk. Recognize
# the final transaction first; never reverse an older patch into newer source.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${INIT_PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE INIT_ALREADY_APPLIED OUTPUT_QUIET ERROR_QUIET)
if(NOT INIT_ALREADY_APPLIED EQUAL 0)
    mdkr_apply_checked_patch("${CLEANUP_PATCH_FILE}")
    mdkr_apply_checked_patch("${INIT_PATCH_FILE}")
endif()
mdkr_apply_checked_patch("${STAGES_PATCH_FILE}")
# Apply certificate verification before authenticating the final WebSocket
# source, including on incremental FetchContent trees. Never infer the entire
# amendment from a matching conditional string somewhere in the file.
mdkr_apply_checked_patch("${PATCH_FILE}")
# Retirement extends Processor's prepared-dispatch API. Recognize the final
# composition first, without reversing an older patch into newer source.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${RETIREMENT_PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE RETIREMENT_ALREADY_APPLIED OUTPUT_QUIET ERROR_QUIET)
if(NOT RETIREMENT_ALREADY_APPLIED EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PREPARED_PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE PREPARED_ALREADY_APPLIED OUTPUT_QUIET ERROR_QUIET)
    if(NOT PREPARED_ALREADY_APPLIED EQUAL 0)
        mdkr_apply_checked_patch("${WORK_PATCH_FILE}")
        mdkr_apply_checked_patch("${PREPARED_PATCH_FILE}")
    endif()
    mdkr_apply_checked_patch("${RETIREMENT_PATCH_FILE}")
endif()
mdkr_apply_checked_patch("${PICOHASH_PATCH_FILE}")
include("${CMAKE_CURRENT_LIST_DIR}/verify_datachannel_startup.cmake")
mdkr_verify_datachannel_startup("${SOURCE_DIR}")

# The Mbed TLS DTLS read callback must report the copied datagram length, not
# the caller's whole buffer capacity, or every incoming handshake flight ends
# in a fatal invalid-record parse. Upstream fixed this in v0.24.4 (our former
# vendored read-length patch, retired at the v0.24.5 bump); the load-bearing
# `return int(bufMin)` content check in cmake/datachannel.cmake still guards
# it at configure time, so nothing is patched here anymore.

set(PLOG_HEADER "${SOURCE_DIR}/deps/plog/include/plog/Log.h")
if(NOT EXISTS "${PLOG_HEADER}")
    message(FATAL_ERROR "libdatachannel plog header is missing")
endif()
file(READ "${PLOG_HEADER}" PLOG_TEXT)
if(NOT PLOG_TEXT MATCHES "PLOG_GET_FUNC\\(\\)[ \t]+__func__")
    set(PLOG_PRETTY "#   define PLOG_GET_FUNC()      __PRETTY_FUNCTION__")
    set(PLOG_PLAIN "#   define PLOG_GET_FUNC()      __func__")
    string(FIND "${PLOG_TEXT}" "${PLOG_PRETTY}" PLOG_PRETTY_OFFSET)
    if(PLOG_PRETTY_OFFSET EQUAL -1)
        message(FATAL_ERROR "Pinned plog function-name definition changed")
    endif()
    string(REPLACE "${PLOG_PRETTY}" "${PLOG_PLAIN}" PLOG_TEXT "${PLOG_TEXT}")
    file(WRITE "${PLOG_HEADER}" "${PLOG_TEXT}")
endif()
