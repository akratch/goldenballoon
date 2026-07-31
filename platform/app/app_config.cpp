// app_config.cpp — see app_config.h.
#include "app_config.h"

#include <SDL.h>

#include <cstdio>
#include <fstream>
#include <map>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::map<std::string, std::string> g_kv;

// Empty string means "no writable pref location" (SDL_GetPrefPath failed). The
// caller treats that as an error rather than falling back to a CWD-relative file
// (a double-clicked .app has CWD=/, so that would silently lose prefs) [AUDIT-0039].
std::string prefsFilePath() {
    char *p = SDL_GetPrefPath("mdkr64", "mdkr64");
    if (!p) return "";
    std::string base = p;
    SDL_free(p);
    return base + "mdkr64_app.ini";
}

// Atomic replace, mirroring config_pc.c replaceConfigFile().
bool atomicReplace(const std::string &tmp, const std::string &path) {
#ifdef _WIN32
    return MoveFileExA(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}
}  // namespace

namespace AppConfig {

void load() {
    g_kv.clear();
    std::ifstream f(prefsFilePath());
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // A stray '\r' from a CRLF file would otherwise become part of the value.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        g_kv[line.substr(0, eq)] = unescapeValue(line.substr(eq + 1));
    }
}

// Atomic + checked persist (AUDIT-0039): write to a temp file, verify every
// write and the flush/close, then atomically replace the live file — a crash or
// full disk mid-write can never truncate the existing prefs. Returns false (and
// leaves the old file intact) on any failure so callers can surface it.
bool save() {
    std::string path = prefsFilePath();
    if (path.empty()) {
        SDL_Log("[app] preferences NOT saved: no writable pref path (SDL_GetPrefPath failed)");
        return false;
    }
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            SDL_Log("[app] preferences save failed: cannot open %s", tmp.c_str());
            return false;
        }
        f << "# mdkr64 app preferences\n";
        // Values are escaped so no value (e.g. a filename with an embedded newline)
        // can inject an extra ini line. Keys are code-controlled literals.
        for (const auto &kv : g_kv) f << kv.first << "=" << escapeValue(kv.second) << "\n";
        f.flush();
        f.close();
        if (f.fail()) {
            SDL_Log("[app] preferences save failed: write/close error on %s", tmp.c_str());
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (!atomicReplace(tmp, path)) {
        SDL_Log("[app] preferences save failed: could not replace %s", path.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string get(const std::string &key, const std::string &fallback) {
    auto it = g_kv.find(key);
    return it != g_kv.end() ? it->second : fallback;
}

void set(const std::string &key, const std::string &value) { g_kv[key] = value; }

}  // namespace AppConfig
