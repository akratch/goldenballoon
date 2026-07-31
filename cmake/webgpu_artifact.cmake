# Explicit wgpu-native prebuilt tuple selection.
#
# Inputs to mdkr_select_wgpu_artifact:
#   operating system: Darwin | Linux | Windows
#   normalized CPU:   arm64/aarch64 | x86_64/amd64
#   compiler ABI:     apple | gnu
#
# Outputs are written to the caller's scope. Unsupported tuples are data, not a
# hidden x86_64 fallback; webgpu.cmake turns them into one actionable error.
function(mdkr_select_wgpu_artifact os cpu abi out_supported out_asset out_sha out_reason)
    string(TOLOWER "${cpu}" _cpu)
    string(TOLOWER "${abi}" _abi)
    set(_supported TRUE)
    set(_asset "")
    set(_sha "")
    set(_reason "")

    if(os STREQUAL "Darwin" AND _abi STREQUAL "apple")
        if(_cpu MATCHES "^(arm64|aarch64)$")
            set(_asset "wgpu-macos-aarch64-release.zip")
            set(_sha "a5797a37b1adf720bcd5dcffb291edbbd5b7b14be0a3874c28e6393a655a7a3e")
        elseif(_cpu MATCHES "^(x86_64|amd64)$")
            set(_asset "wgpu-macos-x86_64-release.zip")
            set(_sha "8e2f7378548ddd0e2cf21e7d864dda46e953f0af724855a33778b85ead206d41")
        else()
            set(_supported FALSE)
        endif()
    elseif(os STREQUAL "Linux" AND _abi STREQUAL "gnu")
        if(_cpu MATCHES "^(arm64|aarch64)$")
            set(_asset "wgpu-linux-aarch64-release.zip")
            set(_sha "015fcdf1dbae82e614a783cc38017e5399ae0927a889fe9b69c9b664bc61b47a")
        elseif(_cpu MATCHES "^(x86_64|amd64)$")
            set(_asset "wgpu-linux-x86_64-release.zip")
            set(_sha "95a4d90c071005a98d03eab348beaa6b07e16eb00d1dcdb9f8348f75eb97ec5a")
        else()
            set(_supported FALSE)
        endif()
    elseif(os STREQUAL "Windows" AND _abi STREQUAL "gnu" AND
           _cpu MATCHES "^(x86_64|amd64)$")
        set(_asset "wgpu-windows-x86_64-gnu-release.zip")
        set(_sha "d471e3614733c1d4ddd61bfd19868356477d0d37bf531bf8c6cb64a7f579bd2a")
    else()
        set(_supported FALSE)
    endif()

    if(NOT _supported)
        set(_reason
            "no pinned wgpu-native prebuilt for OS='${os}', CPU='${cpu}', ABI='${abi}'")
    endif()
    set(${out_supported} "${_supported}" PARENT_SCOPE)
    set(${out_asset} "${_asset}" PARENT_SCOPE)
    set(${out_sha} "${_sha}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()
