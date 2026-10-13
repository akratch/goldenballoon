# Read-only verification of the final composed startup/work amendments. Checking the
# older cleanup patch in reverse alone no longer works after the transaction
# patch deliberately changes its constructor ordering. Exact final hashes bind
# the complete token/helper use, admission gate and selective retirement logic.
# Retirement extends prepared dispatch and factory/transport ownership. Bind all
# final sources, including the prior scheduler and amended Queue accounting.
function(mdkr_verify_datachannel_startup source_dir)
    foreach(pin
            "init.cpp|154ffe98419e21bc10a0798e46f0d066ef500db120000de0ba768fc55573af67"
            "init.hpp|6a2f48f0926a6536c58f919c93a6536e7c367f0fcb28bc92daf58f3b94cdf9e9"
            "threadpool.hpp|f55c1a34aecbc64a5d4331369f45b9fe3754db11ae095585f64462c66b701aac"
            "threadpool.cpp|13486a485a8a52e311bd5b6471fdfbc892afc62da0c2ff1e865174d28f8331a1"
            "processor.hpp|0c7ff93f73fdecfcb34bc1a568d0bca86245de99e1be104ada4212124f009d1e"
            "processor.cpp|a77834e8e35ebfc297643fbf12443ccb475271310270419d9652d365a6c373d5"
            "queue.hpp|6ced0eeaeb7d6b6b8b9707e46aa6cde13bac7f8109472272d91ad9663d60e44e"
            "transport.hpp|5f29aa3c08ab248e5c5c56d4fb5d80903236e2a9692fb44b71c154301b65c077"
            "transport.cpp|44270061c9b8824355746227972cc2dd4369e476b6f23bef824b40eaa8d7cee8"
            "peerconnection.cpp|c78d83d55f7306d686ca45cf85663eca2da5e5a0b7d7d20c8f0ed69855e1eac6"
            "websocket.cpp|d8cf3df6bc98568ba923c560b113b893d51e6a8833a89d603e31efb8e37eda97"
            "icetransport.cpp|12df8a4c490c244af4876c8286d7aa46740792c8701357d61671c5a3fa5d0d6c"
            "icetransport.hpp|8c69d0cbb88cff0a79120b67311402f898888992c166d5a9828c8bb7648d8a56"
            "sctptransport.cpp|9dfb612554762924cdf98665c7b60be5eaadefd9e7afd6687430af3b57a436f6"
            "sctptransport.hpp|0fcfd90fe4dbfa6330026f8d7c5337d2d61835776ee598b6a5a029e7662c5a2e"
            "tcpserver.cpp|27bfdbbc2d48843755b5ed1ae1718e7e02cf735cf0a1a00d456a1399d7399a2a")
        string(REPLACE "|" ";" fields "${pin}")
        list(GET fields 0 name)
        list(GET fields 1 expected)
        if(NOT EXISTS "${source_dir}/src/impl/${name}")
            message(FATAL_ERROR "Missing reviewed RTC startup/work source: ${name}")
        endif()
        file(SHA256 "${source_dir}/src/impl/${name}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR
                "RTC startup/work source ${name} does not match the reviewed composed amendments; "
                "use the patched source also for source-directory overrides")
        endif()
    endforeach()
    file(SHA256 "${source_dir}/deps/libjuice/src/picohash.h" picohash_sha256)
    if(NOT picohash_sha256 STREQUAL
            "6919bc490c56a7ed0cb73cf9f439794309e6463739bf97206cc08763c776d9a5")
        message(FATAL_ERROR
            "libjuice picohash callbacks do not match the reviewed type-safe amendment; "
            "use the patched source also for source-directory overrides")
    endif()
    execute_process(
        COMMAND git apply --reverse --check
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libdatachannel-startup-stages.patch"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE stages_applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT stages_applied EQUAL 0)
        message(FATAL_ERROR "RTC PollService/SCTP startup-stage amendments are absent or incomplete")
    endif()
    # Retirement extends the prepared Processor API; the older patch's reverse
    # context is no longer a final-state check. Exact hashes bind the composed
    # forms; reverse-check the final additive patch. Overrides remain read-only.
    execute_process(
        COMMAND git apply --reverse --check
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libdatachannel-transport-retirement.patch"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE retirement_applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT retirement_applied EQUAL 0)
        message(FATAL_ERROR "RTC prepared dispatch/transport retirement amendments are absent or incomplete")
    endif()
endfunction()
