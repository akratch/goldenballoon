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
#include <shellapi.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include <string>
#include <vector>
#include <algorithm>
#include <iterator>

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

std::wstring toUtf16(const std::string &text) {
    if (text.empty()) return {};
    const int need = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (need <= 1) return {};
    std::vector<wchar_t> buffer(static_cast<size_t>(need));
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1,
            buffer.data(), need) <= 0) return {};
    return std::wstring(buffer.data());
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
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
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

bool openContentPack(std::string &out) {
    // A zipped pack goes through the same reader and the same path validation
    // as an unzipped one, so the archive is what this offers. A pack FOLDER is
    // installed by dropping it in the mods folder -- the common-item dialog
    // cannot pick a file or a directory in one pass, and offering two buttons
    // for one job is what the launcher just stopped doing elsewhere.
    static const wchar_t kFilter[] =
        L"Content packs (*.zip)\0*.zip\0"
        L"All files\0*.*\0"
        L"\0";

    std::vector<wchar_t> file(32768, L'\0');

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile   = file.data();
    ofn.nMaxFile    = (DWORD)file.size();
    ofn.lpstrTitle  = L"Install a content pack";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) return false;

    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool openCharacterSource(std::string &out) {
    static const wchar_t kFilter[] =
        L"Character sources (*.mdkrchar;*.mdkrsource;*.glb;*.dae;*.zip;*.gltf;*.fbx;*.obj;*.blend;*.usd;*.usda;*.usdc;*.usdz;*.ma;*.mb;*.max;*.c4d;*.3ds)\0*.mdkrchar;*.mdkrsource;*.glb;*.dae;*.zip;*.gltf;*.fbx;*.obj;*.blend;*.usd;*.usda;*.usdc;*.usdz;*.ma;*.mb;*.max;*.c4d;*.3ds\0"
        L"Golden Balloon packages (*.mdkrchar)\0*.mdkrchar\0"
        L"Data-only adapter results (*.mdkrsource)\0*.mdkrsource\0"
        L"glTF binary models (*.glb)\0*.glb\0"
        L"COLLADA models (*.dae)\0*.dae\0"
        L"Authoring archives (*.zip)\0*.zip\0"
        L"DCC sources requiring GLB export (*.gltf;*.fbx;*.obj;*.blend;*.usd;*.usda;*.usdc;*.usdz;*.ma;*.mb;*.max;*.c4d;*.3ds)\0*.gltf;*.fbx;*.obj;*.blend;*.usd;*.usda;*.usdc;*.usdz;*.ma;*.mb;*.max;*.c4d;*.3ds\0"
        L"\0";
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Import a custom character";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool openCharacterLicense(std::string &out) {
    static const wchar_t kFilter[] =
        L"License and notice files\0LICENSE*;COPYING*;NOTICE*;*.txt;*.md\0"
        L"All files\0*.*\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Choose the character license or notice file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool openPortraitImage(std::string &out) {
    static const wchar_t kFilter[] = L"PNG images\0*.png\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Choose character portrait artwork";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool openCharacterDraftBundle(std::string &out) {
    static const wchar_t kFilter[] =
        L"Golden Balloon draft bundles\0*.mdkrdrafts\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrTitle = L"Review a character draft bundle";
    ofn.lpstrDefExt = L"mdkrdrafts";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterConvertedGlb(std::string &out) {
    static const wchar_t kFilter[] = L"glTF binary models\0*.glb\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"converted-character.glb";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Save converted character model";
    ofn.lpstrDefExt = L"glb";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterCapture(std::string &out) {
    static const wchar_t kFilter[] = L"PNG images\0*.png\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"character-inspection.png";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Save a custom character inspection";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveGameProgress(std::string &out, const std::string &suggestedName) {
    static const wchar_t kFilter[] = L"Golden Balloon saves\0*.mdkr-save\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const auto initial = toUtf16(suggestedName);
    if (initial.empty() || initial.size() >= file.size()) return false;
    std::copy(initial.begin(), initial.end(), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Export game save";
    ofn.lpstrDefExt = L"mdkr-save";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterReport(std::string &out) {
    static const wchar_t kFilter[] = L"HTML documents\0*.html\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"character-visual-report.html";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Export custom character visual report";
    ofn.lpstrDefExt = L"html";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterDeviceProfile(std::string &out) {
    static const wchar_t kFilter[] = L"JSON documents\0*.json\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"character-device-profile.json";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrTitle = L"Export custom character device profile";
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterPackage(std::string &out) {
    static const wchar_t kFilter[] =
        L"Golden Balloon character packages\0*.mdkrchar\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"custom-character.mdkrchar";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Export custom character package";
    ofn.lpstrDefExt = L"mdkrchar";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterDraftBundle(std::string &out) {
    static const wchar_t kFilter[] =
        L"Golden Balloon draft bundles\0*.mdkrdrafts\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"character-drafts.mdkrdrafts";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrTitle = L"Export character named drafts";
    ofn.lpstrDefExt = L"mdkrdrafts";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterDiagnostic(std::string &out) {
    static const wchar_t kFilter[] = L"JSON documents\0*.json\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"character-import-diagnostic.json";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Export failed-import diagnostic";
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool revealInFileManager(const std::string &path) {
    const std::wstring wide = toUtf16(path);
    if (wide.empty()) return false;
    /* Windows filenames cannot contain a quote, so the quoted /select value
     * cannot be split into another explorer argument. The file is selected,
     * never opened through its association. */
    const std::wstring arguments = L"/select,\"" + wide + L"\"";
    /* Fully qualified, because unqualified executable resolution consults the
     * CWD -- and this file already treats the CWD as untrusted, setting
     * OFN_NOCHANGEDIR on every dialog for exactly that reason. A bare
     * "explorer.exe" meant that a planted binary in the working directory ran
     * with the user's token the first time they picked "Show in folder" on a
     * downloaded character package. GetWindowsDirectoryW is used rather than
     * expanding %SystemRoot%, so the path cannot be redirected by an
     * environment variable either. */
    wchar_t windows_dir[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    if (length == 0u || length >= MAX_PATH) return false;
    std::wstring explorer(windows_dir, length);
    if (!explorer.empty() && explorer.back() != L'\\') explorer.push_back(L'\\');
    explorer += L"explorer.exe";
    const HINSTANCE launched = ShellExecuteW(
        nullptr, L"open", explorer.c_str(), arguments.c_str(), nullptr,
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(launched) > 32;
}

}  // namespace filedialog
