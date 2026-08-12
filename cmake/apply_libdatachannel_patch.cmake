if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "SOURCE_DIR and PATCH_FILE are required")
endif()

set(WEBSOCKET_SOURCE "${SOURCE_DIR}/src/impl/websocket.cpp")
if(NOT EXISTS "${WEBSOCKET_SOURCE}")
    message(FATAL_ERROR "libdatachannel WebSocket source is missing")
endif()
file(READ "${WEBSOCKET_SOURCE}" WEBSOCKET_TEXT)
if(WEBSOCKET_TEXT MATCHES "defined\\(_WIN32\\) && !USE_MBEDTLS")
    return()
endif()

find_program(GIT_EXECUTABLE git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE PATCH_RESULT
    ERROR_VARIABLE PATCH_ERROR)
if(NOT PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not apply libdatachannel security patch: ${PATCH_ERROR}")
endif()
