if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PATCH_FILE OR
   NOT DEFINED DTLS_PATCH_FILE)
    message(FATAL_ERROR
        "SOURCE_DIR, PATCH_FILE and DTLS_PATCH_FILE are required")
endif()

set(WEBSOCKET_SOURCE "${SOURCE_DIR}/src/impl/websocket.cpp")
if(NOT EXISTS "${WEBSOCKET_SOURCE}")
    message(FATAL_ERROR "libdatachannel WebSocket source is missing")
endif()
file(READ "${WEBSOCKET_SOURCE}" WEBSOCKET_TEXT)
if(NOT WEBSOCKET_TEXT MATCHES "defined\\(_WIN32\\) && !USE_MBEDTLS")
    find_program(GIT_EXECUTABLE git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE PATCH_RESULT
        ERROR_VARIABLE PATCH_ERROR)
    if(NOT PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not apply libdatachannel security patch: ${PATCH_ERROR}")
    endif()
endif()

# The pinned release's Mbed TLS DTLS read callback reports the caller's whole
# buffer capacity instead of the copied datagram length, which makes every
# incoming handshake flight end in a fatal invalid-record parse; a browser's
# very first ClientHello kills the connection. Fixed to report the copied
# length; see cmake/patches/libdatachannel-mbedtls-dtls-read-length.patch.
set(DTLS_SOURCE "${SOURCE_DIR}/src/impl/dtlstransport.cpp")
if(NOT EXISTS "${DTLS_SOURCE}")
    message(FATAL_ERROR "libdatachannel DTLS transport source is missing")
endif()
file(READ "${DTLS_SOURCE}" DTLS_TEXT)
if(NOT DTLS_TEXT MATCHES "return int\\(bufMin\\)")
    find_program(GIT_EXECUTABLE git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${DTLS_PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE DTLS_PATCH_RESULT
        ERROR_VARIABLE DTLS_PATCH_ERROR)
    if(NOT DTLS_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Could not apply libdatachannel DTLS read-length patch: "
            "${DTLS_PATCH_ERROR}")
    endif()
endif()

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
