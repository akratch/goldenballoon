include("${CMAKE_CURRENT_LIST_DIR}/../cmake/webgpu_artifact.cmake")

function(expect_supported os cpu abi asset)
    mdkr_select_wgpu_artifact("${os}" "${cpu}" "${abi}" ok got sha reason)
    if(NOT ok OR NOT got STREQUAL asset OR sha STREQUAL "")
        message(FATAL_ERROR
            "expected ${os}/${cpu}/${abi} -> ${asset}; got ok=${ok}, asset=${got}, reason=${reason}")
    endif()
endfunction()

function(expect_unsupported os cpu abi)
    mdkr_select_wgpu_artifact("${os}" "${cpu}" "${abi}" ok asset sha reason)
    if(ok OR NOT asset STREQUAL "" OR NOT sha STREQUAL "" OR reason STREQUAL "")
        message(FATAL_ERROR
            "expected explicit rejection for ${os}/${cpu}/${abi}; got ok=${ok}, asset=${asset}")
    endif()
endfunction()

expect_supported(Darwin arm64 apple wgpu-macos-aarch64-release.zip)
expect_supported(Darwin x86_64 apple wgpu-macos-x86_64-release.zip)
expect_supported(Linux aarch64 gnu wgpu-linux-aarch64-release.zip)
expect_supported(Linux x86_64 gnu wgpu-linux-x86_64-release.zip)
expect_supported(Windows AMD64 gnu wgpu-windows-x86_64-gnu-release.zip)

expect_unsupported(Darwin ppc64 apple)
expect_unsupported(Linux riscv64 gnu)
expect_unsupported(Linux armv7 gnu)
expect_unsupported(Windows arm64 gnu)
expect_unsupported(Windows x86_64 msvc)
expect_unsupported(Plan9 x86_64 gnu)

message(STATUS "check_webgpu_artifacts: PASS")
