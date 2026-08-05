#include "app_restart.h"

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)
bool utf8ToWide(const char *value, std::wstring &wide) {
    if (value == nullptr) value = "";
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (count <= 0) return false;
    wide.resize(static_cast<size_t>(count));
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, wide.data(), count) <= 0) {
        wide.clear();
        return false;
    }
    wide.resize(static_cast<size_t>(count - 1));
    return true;
}

bool wideToUtf8(const wchar_t *value, std::string &utf8) {
    if (value == nullptr) return false;
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    utf8.resize(static_cast<size_t>(count));
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, utf8.data(), count,
            nullptr, nullptr) <= 0) {
        utf8.clear();
        return false;
    }
    utf8.resize(static_cast<size_t>(count - 1));
    return true;
}
#endif

bool setEnvironment(const char *name, const char *value) {
#if defined(_WIN32)
    std::wstring wideName;
    std::wstring wideValue;
    const char *text = value ? value : "";
    if (!utf8ToWide(name, wideName) || !utf8ToWide(text, wideValue) ||
        _wputenv_s(wideName.c_str(), wideValue.c_str()) != 0) {
        return false;
    }
    /* Keep the narrow CRT copy coherent for the ASCII control variables used
     * by main_app's ordinary getenv dispatch. UTF-8 ROM paths deliberately
     * remain owned by the wide environment and are decoded below. Clearing is
     * always safe in both copies. */
    bool ascii = true;
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(text);
         *cursor != '\0'; ++cursor) {
        if (*cursor > 0x7fu) {
            ascii = false;
            break;
        }
    }
    return (!text[0] || ascii) ? _putenv_s(name, text) == 0 : true;
#else
    if (value == nullptr || value[0] == '\0') {
        return unsetenv(name) == 0;
    }
    return setenv(name, value, 1) == 0;
#endif
}

bool getEnvironment(const char *name, std::string &value) {
#if defined(_WIN32)
    std::wstring wideName;
    if (!utf8ToWide(name, wideName)) return false;
    return wideToUtf8(_wgetenv(wideName.c_str()), value);
#else
    const char *found = std::getenv(name);
    if (found == nullptr) return false;
    value = found;
    return true;
#endif
}

void clearHandoff() {
    (void)setEnvironment("MDKR_APP_RESTART_GAME", "");
    (void)setEnvironment("MDKR_APP_AUTOPLAY", "");
    (void)setEnvironment("MDKR_ROM", "");
}

void clearAutomationControls() {
    /* Read only by the autoplay harness. A player's restart must not inherit
     * an automation tick budget, a scripted input file, or a one-shot video
     * override from whatever environment the app was launched with. */
    (void)setEnvironment("MDKR_APP_AUTOPLAY_TICKS", "");
    (void)setEnvironment("MDKR_APP_AUTOPLAY_INPUT_SCRIPT", "");
    (void)setEnvironment("MDKR_APP_AUTOPLAY_VIDEO_SET", "");
}

}  // namespace

bool AppRestart_setEnv(const char *name, const char *value) {
    return setEnvironment(name, value);
}

bool AppRestart_getEnv(const char *name, std::string &value) {
    return getEnvironment(name, value);
}

bool AppRestart_stageGame(const char *romPath, bool autoplaySession) {
    if (romPath == nullptr || romPath[0] == '\0') {
        return false;
    }
    clearHandoff();
    /* Narrow deterministic failure seam for the real process-recovery test.
     * It is deliberately evaluated after clearing, so a forced failure proves
     * that no stale marker can create a relaunch loop. */
    if (std::getenv("MDKR_APP_TEST_RESTART_STAGE_FAILURE") != nullptr) {
        return false;
    }
    if (!autoplaySession) {
        clearAutomationControls();
    }
    if (!setEnvironment("MDKR_ROM", romPath) ||
        (autoplaySession && !setEnvironment("MDKR_APP_AUTOPLAY", "1")) ||
        !setEnvironment("MDKR_APP_RESTART_GAME", "1")) {
        clearHandoff();
        return false;
    }
    return true;
}

bool AppRestart_pendingGame() {
    std::string marker;
    return getEnvironment("MDKR_APP_RESTART_GAME", marker) && marker == "1";
}

bool AppRestart_consumeGame(std::string &romPath) {
    std::string marker;
    if (!getEnvironment("MDKR_APP_RESTART_GAME", marker) || marker != "1") {
        return false;
    }
    if (!getEnvironment("MDKR_ROM", romPath)) romPath.clear();
    clearHandoff();
    return !romPath.empty();
}

void AppRestart_clear() {
    clearHandoff();
}
