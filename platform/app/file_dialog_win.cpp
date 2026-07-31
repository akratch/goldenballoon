// file_dialog_win.cpp — GetOpenFileNameW. See file_dialog.h.
//
// comdlg32 rather than the newer COM IFileOpenDialog on purpose: comdlg32.dll
// is a stock Windows system DLL present on every install, whereas the COM route
// would pull ole32/shell32 initialization into the shell for no user-visible
// gain here. See the import-table note in docs/APP_SHELL.md — comdlg32.dll is a
// deliberate, documented addition to the windows-validate allowlist.
#include "file_dialog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#include <string>
#include <vector>

namespace filedialog {

namespace {

// UTF-16 -> UTF-8, so the rest of the shell keeps working in char paths.
std::string toUtf8(const wchar_t *w) {
    if (w == nullptr || w[0] == L'\0') return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::vector<char> buf((size_t)need);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf.data(), need, nullptr, nullptr) <= 0) {
        return std::string();
    }
    return std::string(buf.data());
}

}  // namespace

bool isAvailable() { return true; }

bool openRom(std::string &out) {
    // Double-NUL-terminated filter pairs, as the Win32 API expects.
    static const wchar_t kFilter[] =
        L"Nintendo 64 ROM images\0*.z64;*.n64;*.v64\0"
        L"All files\0*.*\0"
        L"\0";

    std::vector<wchar_t> file(32768, L'\0');   // room for a long path

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;   // app-modal; the launcher has one window
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile   = file.data();
    ofn.nMaxFile    = (DWORD)file.size();
    ofn.lpstrTitle  = L"Choose a Nintendo 64 ROM";
    // NOCHANGEDIR matters: without it the dialog mutates the process working
    // directory, which would silently change how every later relative path in
    // the app resolves.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) {
        return false;   // cancelled, or the dialog failed — `out` untouched
    }

    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

}  // namespace filedialog
