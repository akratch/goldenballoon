// app_config.h — app-shell preferences (last ROM, window size, ...).
//
// Distinct from the engine's ge007.ini. Persisted as mdkr64_app.ini in the
// per-OS prefs directory (SDL_GetPrefPath): %APPDATA% on Windows,
// ~/Library/Application Support on macOS, ~/.local/share on Linux.
#ifndef MDKR_APP_CONFIG_H
#define MDKR_APP_CONFIG_H

#include <string>

namespace AppConfig {

// A replace can be visible to this process even when the filesystem cannot
// confirm that the containing directory metadata reached durable storage.
// Keep that state separate from a write failure so callers never claim that
// an applied preference was discarded.
enum class PersistResult {
    Failed,
    Durable,
    DurabilityUnconfirmed,
};

inline bool persistResultApplied(PersistResult result) {
    return result == PersistResult::Durable ||
           result == PersistResult::DurabilityUnconfirmed;
}

void load();
#ifdef MDKR_APP_CONFIG_TESTING
// Test-only direct path seam for failures that SDL_GetPrefPath cannot
// deterministically produce on every host.
bool loadPathForTest(const std::string &path);
// Forces the post-replace directory durability barrier to fail. The atomic
// replacement remains visible, matching filesystems that reject directory
// fsync after a successful rename.
void forceDirectorySyncFailureForTest(bool enabled);
#endif
// Persist prefs atomically. Failed means the previous preference file remains
// authoritative; DurabilityUnconfirmed means the replacement is visible now
// but must be saved again before promising it survives a restart.
PersistResult save();
std::string get(const std::string &key, const std::string &fallback = "");
void set(const std::string &key, const std::string &value);

// Change one preference and persist it as a single transaction. See save().
PersistResult setAndSave(const std::string &key, const std::string &value);

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
