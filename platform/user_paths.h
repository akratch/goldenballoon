/* Native packaged-resource and mutable-user-data path policy.
 *
 * A macOS app bundle is immutable after signing.  The app entry point registers
 * its executable before any engine/config initialization; this module then
 * resolves immutable Resources absolutely and mutable state below SDL's
 * per-user preference directory.  Non-packaged command-line builds retain
 * their historical CWD-relative paths so test runs remain isolated.
 */
#ifndef MDKR64_USER_PATHS_H
#define MDKR64_USER_PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 for a configured packaged app, 0 for a non-packaged executable,
 * and -1 when a package was recognized but SDL's writable preference directory
 * or a required legacy migration could not be prepared. */
int mdkr_user_paths_init(const char *executable_path);

/* Re-run the copy-only migration policy. Primarily useful to prove idempotence;
 * destinations that already exist are never replaced. */
int mdkr_user_paths_prepare_packaged_data(void);

/* Human-readable detail for the most recent packaged-path failure. The pointer
 * has process lifetime and is empty when no more specific recovery exists. */
const char *mdkr_user_paths_last_error(void);

int mdkr_user_paths_is_packaged(void);

/* Portable mode: a file named `portable.txt` sits in the same directory as the
 * running executable. Config (mdkr64.ini), save/, and mods/ then resolve next
 * to the executable instead of the per-user home directory, sidestepping a home
 * path the operating system cannot represent (e.g. a Windows account name with
 * non-ASCII characters, issue #33). Detection is lazy and cross-platform: the
 * first path query resolves it. Environment overrides still win over it. */
int mdkr_user_paths_is_portable(void);

/* Safety net for a first-run player whose per-user home directory cannot be
 * written and who has not placed a portable.txt. A config/settings writer calls
 * this after its normal write to the home directory has FAILED. It resolves the
 * executable's own directory (the same location portable.txt selects) and, when
 * one is available, activates it for every subsequent path query and returns 1.
 * The caller should then re-resolve its paths and retry the write once. Returns
 * 0 when no relocation is possible or already in effect (portable mode, or the
 * executable directory could not be resolved), so the caller does not loop. */
int mdkr_user_paths_activate_write_fallback(void);

/* 1 once a settings write has been relocated next to the executable because the
 * home directory was not writable (never for an explicit portable.txt). Drives
 * the player-facing "settings were saved next to the game instead" notice. */
int mdkr_user_paths_write_relocated(void);

/* When config/save are being relocated next to the executable (portable.txt or
 * an activated write fallback), copy that base directory into `output` and
 * return 1; otherwise return 0. Lets the launcher keep its own preferences file
 * (mdkr64_app.ini) beside the game's config rather than in the home directory
 * the operating system could not represent. */
int mdkr_user_paths_relocation_base(char *output, size_t output_size);

/* The directory the app shell keeps mdkr64.log in (platform/app/diag_log.cpp
 * names the file inside it). Honours MDKR_APP_PREFS_DIR exactly as the logger
 * does, then the per-user preference root. 1 on success. */
int mdkr_user_log_directory(char *output, size_t output_size);

int mdkr_user_video_config_path(char *output, size_t output_size);
int mdkr_user_save_directory(char *output, size_t output_size);

/* One-word origin of what mdkr_user_save_directory() resolves right now:
 * "env", "portable", "fallback", "legacy", "per-user", "cwd", or "browser".
 * Drives the boot log line so a support log answers "where do my saves go?" at
 * a glance (issue #54). The returned pointer is a string literal. */
const char *mdkr_user_paths_save_origin_label(void);

/* Issue #54 save-failure surfacing. The save layer (eeprom/pak/ghost) calls the
 * note function when a durable write has failed and no relocation rescued it;
 * the latch is set once per session (the first failure's directory is kept).
 * The app shell polls _failed() and renders one player-facing notice. */
void mdkr_user_paths_note_save_write_failure(const char *directory);
int mdkr_user_paths_save_write_failed(void);
int mdkr_user_paths_save_write_failed_directory(char *output, size_t output_size);
/* The content-pack root (`mods/`). Same policy as the save directory and for
 * the same reason: a signed bundle cannot host player-installed content, so a
 * packaged app looks beside its writable save directory, and a command-line
 * build stays CWD-relative. MDKR_MODS_DIR overrides all of it, like
 * MDKR_SAVE_DIR: automated runs point it at an empty private folder so a pack
 * installed into the folder a developer launches from cannot reach a test. */
int mdkr_user_mods_directory(char *output, size_t output_size);
/* Where that folder came from: "env", "portable", "fallback", "per-user",
 * "cwd" (a command-line build), or "browser". For the boot log. */
const char *mdkr_user_paths_mods_origin_label(void);
/* Locally imported custom characters and their disposable compiled caches.
 * Separate from mods/: ordinary content packs are read-only override sources,
 * while character import is a launcher-owned transaction with provenance and
 * cache invalidation. Nothing from this directory is bundled or synchronized. */
int mdkr_user_characters_directory(char *output, size_t output_size);
int mdkr_user_resource_path(const char *relative_path,
                            char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_USER_PATHS_H */
