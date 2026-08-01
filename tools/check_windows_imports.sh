#!/usr/bin/env bash
# Fail if a Windows release binary imports anything outside the stock OS.
set -euo pipefail

die() {
  printf 'check_windows_imports: FAIL — %s\n' "$*" >&2
  exit 1
}

filter_bad_imports() {
  local system_dlls
  system_dlls='^('
  system_dlls+='KERNEL32|USER32|SHELL32|GDI32|ADVAPI32|NTDLL|USERENV|'
  system_dlls+='ole32|OLEAUT32|COMBASE|RPCRT4|PROPSYS|COMDLG32|COMCTL32|SHLWAPI|'
  system_dlls+='WS2_32|IPHLPAPI|BCRYPTPRIMITIVES|BCRYPT|CRYPT32|'
  system_dlls+='IMM32|SETUPAPI|VERSION|WINMM|CFGMGR32|HID|POWRPROF|DWMAPI|UXTHEME|DWRITE|'
  system_dlls+='OPENGL32|DINPUT8|d3dcompiler_47|dxgi|D3D12|MSVCRT|UCRTBASE'
  system_dlls+=')\.dll$|^api-ms-win-.*\.dll$'
  grep -viE "${system_dlls}" || true
}

if [[ "${1:-}" == "--self-test" ]]; then
  safe_imports=$'KERNEL32.dll\nMSVCRT.dll\nDINPUT8.dll\nD3D12.dll\napi-ms-win-core-file-l1-1-0.dll'
  [[ -z "$(printf '%s\n' "${safe_imports}" | filter_bad_imports)" ]] ||
    die "stock-system positive control was rejected"
  unsafe_imports=$'SDL2.dll\nlibgcc_s_seh-1.dll'
  rejected="$(printf '%s\n' "${unsafe_imports}" | filter_bad_imports)"
  [[ "${rejected}" == "${unsafe_imports}" ]] ||
    die "non-system broken control escaped the allowlist"
  printf 'check_windows_imports: self-test PASS\n'
  exit 0
fi

[[ $# -eq 1 ]] || die "usage: $0 WINDOWS.exe"
binary="$1"
[[ -f "${binary}" ]] || die "binary not found: ${binary}"
objdump_bin="${OBJDUMP:-objdump}"
command -v "${objdump_bin}" >/dev/null 2>&1 ||
  die "required tool not found: ${objdump_bin}"

if ! dump="$("${objdump_bin}" -p "${binary}" 2>&1)"; then
  printf '%s\n' "${dump}" >&2
  die "could not inspect PE imports"
fi
imports="$(printf '%s\n' "${dump}" | awk '/DLL Name/{print $3}')"
[[ -n "${imports}" ]] || die "PE import table was empty or unreadable"
bad_imports="$(printf '%s\n' "${imports}" | filter_bad_imports)"
if [[ -n "${bad_imports}" ]]; then
  printf '%s\n' "${bad_imports}" >&2
  die "non-system DLL import found; the portable package must be one executable"
fi

printf 'check_windows_imports: PASS — stock Windows DLLs only\n'
