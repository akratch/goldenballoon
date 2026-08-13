// app_config.cpp — see app_config.h.
#include "app_config.h"
#include "fs_utf8.h"
#include "user_paths.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
std::map<std::string, std::string> g_kv;
std::set<std::string> g_dirtyKeys;
std::mutex g_saveMutex;
unsigned long g_tempSerial = 0;
#ifdef MDKR_APP_CONFIG_TESTING
bool g_forceDirectorySyncFailure = false;
#endif
/* A supported Windows path can be 32,767 UTF-16 units and up to four bytes
 * per unit in UTF-8. Escaping backslashes can double it, so this is deliberately
 * above 256 KiB; keep the whole preference file finite despite that contract. */
constexpr size_t kMaxPrefsBytes = 512u * 1024u;
constexpr size_t kMaxPrefsLineBytes = 384u * 1024u;

// Empty string means "no writable pref location" (SDL_GetPrefPath failed). The
// caller treats that as an error rather than falling back to a CWD-relative file
// (a double-clicked .app has CWD=/, so that would silently lose prefs) [AUDIT-0039].
std::string prefsFilePath() {
    // MDKR_APP_PREFS_DIR: test-only isolation, mirroring MDKR_SAVE_DIR
    // (platform/stubs_dkr.c). SDL_GetPrefPath("mdkr64","mdkr64") is one
    // location shared by every local build and every concurrent test run; a
    // shell smoke that drops a ROM (tests/check_shell_dropfile.py, Q2) ends up
    // on the same RomPanel_setRom() -> AppConfig::save() path a real accepted
    // ROM takes, and must not read or overwrite whatever a real player (or
    // another suite) already has there. No-op unless set — production behavior
    // and path are unchanged. The caller-provided directory must already
    // exist; this does not create one, same contract as SDL_GetPrefPath.
    const char *dir = std::getenv("MDKR_APP_PREFS_DIR");
    if (dir != nullptr && dir[0] != '\0') {
        std::string base = dir;
        if (base.back() != '/' && base.back() != '\\') base += '/';
        return base + "mdkr64_app.ini";
    }
    // Portable mode (portable.txt beside the executable) or an activated
    // write-fallback keeps the launcher's own preferences beside the game's
    // config, sidestepping a home path the OS cannot represent (issue #33).
    char relocation[4096];
    if (mdkr_user_paths_relocation_base(relocation, sizeof(relocation))) {
        std::string base = relocation;
        if (base.back() != '/' && base.back() != '\\') base += '/';
        return base + "mdkr64_app.ini";
    }
    char *p = SDL_GetPrefPath("mdkr64", "mdkr64");
    if (!p) return "";
    std::string base = p;
    SDL_free(p);
    return base + "mdkr64_app.ini";
}

// Atomic replace, mirroring config_pc.c replaceConfigFile().
bool atomicReplace(const std::string &tmp, const std::string &path) {
    return mdkr_move_utf8(tmp.c_str(), path.c_str(), 1, 1) == 0;
}

void parseLine(std::map<std::string, std::string> &values, std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') return;
    const auto eq = line.find('=');
    if (eq == std::string::npos) return;
    values[line.substr(0, eq)] = AppConfig::unescapeValue(line.substr(eq + 1));
}

bool loadValues(const std::string &path, std::map<std::string, std::string> &values) {
    if (path.empty()) {
        SDL_Log("[app] preferences ignored: no readable preference path");
        return false;
    }
    std::FILE *file = mdkr_fopen_utf8(path.c_str(), "rb");
    if (!file) {
        if (errno == ENOENT) return true;  // A missing preferences file is normal first run.
        SDL_Log("[app] preferences ignored: could not open %s", path.c_str());
        return false;
    }
    char chunk[1024];
    std::string pending;
    size_t total = 0;
    size_t chunkBytes;
    while ((chunkBytes = std::fread(chunk, 1, sizeof(chunk), file)) != 0u) {
        if (chunkBytes > kMaxPrefsBytes - total) {
            SDL_Log("[app] preferences ignored: %s is too large", path.c_str());
            std::fclose(file);
            return false;
        }
        total += chunkBytes;
        for (size_t index = 0u; index < chunkBytes; ++index) {
            const char byte = chunk[index];
            /* Preferences are a line-oriented UTF-8-ish text format. A NUL
             * cannot be represented by its escaping grammar; reject it rather
             * than letting a C-string helper undercount a hostile file. */
            if (byte == '\0') {
                SDL_Log("[app] preferences ignored: %s contains a NUL byte", path.c_str());
                std::fclose(file);
                return false;
            }
            if (byte == '\n') {
                parseLine(values, pending);
                pending.clear();
                continue;
            }
            if (pending.size() >= kMaxPrefsLineBytes) {
                SDL_Log("[app] preferences ignored: %s has an overlong line", path.c_str());
                std::fclose(file);
                return false;
            }
            pending.push_back(byte);
        }
    }
    if (std::ferror(file)) {
        SDL_Log("[app] preferences ignored: could not read %s", path.c_str());
        std::fclose(file);
        return false;
    }
    if (!pending.empty()) parseLine(values, pending);
    std::fclose(file);
    return true;
}

long processId() {
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

bool openUniqueTemp(const std::string &path, std::string &tmp, std::FILE **out) {
    for (unsigned attempt = 0; attempt < 64u; ++attempt) {
        tmp = path + ".tmp." + std::to_string(processId()) + "." +
              std::to_string(++g_tempSerial);
        *out = mdkr_fopen_utf8(tmp.c_str(), "wbx");
        if (*out != nullptr) return true;
        if (errno != EEXIST) break;
    }
    SDL_Log("[app] preferences save failed: could not create exclusive staging file beside %s",
            path.c_str());
    return false;
}

int syncParentDirectory(const std::string &path) {
#ifdef MDKR_APP_CONFIG_TESTING
    if (g_forceDirectorySyncFailure) {
        errno = EIO;
        return -1;
    }
#endif
    return mdkr_parent_directory_sync_utf8(path.c_str());
}

AppConfig::PersistResult saveValues(
    const std::string &path, const std::map<std::string, std::string> &values) {
    std::string tmp;
    std::FILE *f = nullptr;
    if (!openUniqueTemp(path, tmp, &f)) return AppConfig::PersistResult::Failed;
    bool failed = std::fputs("# mdkr64 app preferences\n", f) < 0;
    for (const auto &kv : values) {
        const std::string line = kv.first + "=" + AppConfig::escapeValue(kv.second) + "\n";
        if (std::fwrite(line.data(), 1, line.size(), f) != line.size()) {
            failed = true;
            break;
        }
    }
    if (!failed && mdkr_file_sync(f) != 0) failed = true;
    if (std::fclose(f) != 0) failed = true;
    if (failed) {
        SDL_Log("[app] preferences save failed: write/sync/close error on %s", tmp.c_str());
        mdkr_remove_utf8(tmp.c_str());
        return AppConfig::PersistResult::Failed;
    }
    if (!atomicReplace(tmp, path)) {
        SDL_Log("[app] preferences save failed: could not replace %s", path.c_str());
        mdkr_remove_utf8(tmp.c_str());
        return AppConfig::PersistResult::Failed;
    }
    if (syncParentDirectory(path) != 0) {
        /* The replacement is visible and data was synced before it. Retain it,
         * but leave a durable diagnostic for filesystems that cannot sync dirs. */
        SDL_Log("[app] preferences saved, but directory durability was not confirmed for %s",
                path.c_str());
        return AppConfig::PersistResult::DurabilityUnconfirmed;
    }
    return AppConfig::PersistResult::Durable;
}

// One locked read-modify-write against `path`. Caller holds g_saveMutex. When
// `setKey` is non-null the pair is applied on top of the merged dirty keys,
// serving setAndSave() without duplicating the transaction.
AppConfig::PersistResult persistAtLocked(const std::string &path,
                                         const std::string *setKey,
                                         const std::string *setValue) {
    const std::string lockPath = path + ".lock";
    MdkrFileLock lock = {(intptr_t)-1};
    if (mdkr_file_lock_acquire_utf8(lockPath.c_str(), &lock) != 0) {
        SDL_Log("[app] preferences save failed: could not lock %s", path.c_str());
        return AppConfig::PersistResult::Failed;
    }
    std::map<std::string, std::string> candidate;
    const bool read = loadValues(path, candidate);
    if (read) {
        for (const std::string &key : g_dirtyKeys) {
            const auto found = g_kv.find(key);
            if (found != g_kv.end()) candidate[key] = found->second;
            else candidate.erase(key);
        }
        if (setKey != nullptr) candidate[*setKey] = *setValue;
    }
    const AppConfig::PersistResult result =
        read ? saveValues(path, candidate) : AppConfig::PersistResult::Failed;
    if (AppConfig::persistResultApplied(result)) {
        g_kv = candidate;
        g_dirtyKeys.clear();
    }
    mdkr_file_lock_release(&lock);
    return result;
}

// Persist with the issue #33 safety net: if the home-directory write fails and
// no portable.txt is present, relocate next to the executable and retry once.
// Caller holds g_saveMutex.
AppConfig::PersistResult persistWithFallbackLocked(const std::string *setKey,
                                                   const std::string *setValue) {
    const std::string path = prefsFilePath();
    if (path.empty()) {
        SDL_Log("[app] preferences NOT saved: no writable pref path (SDL_GetPrefPath failed)");
        return AppConfig::PersistResult::Failed;
    }
    AppConfig::PersistResult result = persistAtLocked(path, setKey, setValue);
    if (result == AppConfig::PersistResult::Failed &&
        mdkr_user_paths_activate_write_fallback()) {
        const std::string relocated = prefsFilePath();
        if (!relocated.empty() && relocated != path) {
            SDL_Log("[app] the settings folder was not writable; saving "
                    "preferences next to the game instead (%s)",
                    relocated.c_str());
            result = persistAtLocked(relocated, setKey, setValue);
        }
    }
    return result;
}
}  // namespace

namespace AppConfig {

void load() {
    std::map<std::string, std::string> loaded;
    const std::string path = prefsFilePath();
    if (loadValues(path, loaded)) {
        g_kv.swap(loaded);
        g_dirtyKeys.clear();
    }
}

#ifdef MDKR_APP_CONFIG_TESTING
bool loadPathForTest(const std::string &path) {
    std::map<std::string, std::string> loaded;
    if (!loadValues(path, loaded)) return false;
    g_kv.swap(loaded);
    g_dirtyKeys.clear();
    return true;
}

void forceDirectorySyncFailureForTest(bool enabled) {
    g_forceDirectorySyncFailure = enabled;
}
#endif

// Atomic + checked persist (AUDIT-0039): write to a temp file, verify every
// write and the flush/close, then atomically replace the live file — a crash or
// full disk mid-write can never truncate the existing prefs. A post-replace
// directory-sync fault is deliberately reported as visible but unconfirmed.
PersistResult save() {
    std::lock_guard<std::mutex> guard(g_saveMutex);
    return persistWithFallbackLocked(nullptr, nullptr);
}

std::string get(const std::string &key, const std::string &fallback) {
    auto it = g_kv.find(key);
    return it != g_kv.end() ? it->second : fallback;
}

void set(const std::string &key, const std::string &value) {
    g_kv[key] = value;
    g_dirtyKeys.insert(key);
}

PersistResult setAndSave(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> guard(g_saveMutex);
    return persistWithFallbackLocked(&key, &value);
}

}  // namespace AppConfig
