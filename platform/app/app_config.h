// app_config.h — app-shell preferences (last ROM, window size, ...).
//
// Distinct from the engine's ge007.ini. Persisted as mdkr64_app.ini in the
// per-OS prefs directory (SDL_GetPrefPath): %APPDATA% on Windows,
// ~/Library/Application Support on macOS, ~/.local/share on Linux.
#ifndef MDKR_APP_CONFIG_H
#define MDKR_APP_CONFIG_H

#include <string>

namespace AppConfig {

void load();
// Persist prefs atomically. Returns false (old file left intact) if there is no
// writable pref path or the write/replace fails [AUDIT-0039]. Callers may ignore
// the result, but it is available to surface a save failure.
bool save();
std::string get(const std::string &key, const std::string &fallback = "");
void set(const std::string &key, const std::string &value);

// Escape a value to a single injection-safe ini line: backslash-escapes '\',
// '\n' and '\r' so a value can never introduce a new line (and thus never inject
// a forged key=value on reload). unescapeValue is the exact inverse. Applied by
// save()/load() to every value. advanced_env legitimately holds multi-line text
// (KEY=VALUE per line); escaping preserves that intent across the round-trip
// while neutralizing the ini-injection the raw flatten-on-save used to allow.
// Exposed (SDL-free, in app_config_escape.cpp) for direct unit testing.
std::string escapeValue(const std::string &value);
std::string unescapeValue(const std::string &value);

}  // namespace AppConfig

#endif  // MDKR_APP_CONFIG_H
