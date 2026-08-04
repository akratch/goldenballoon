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
int mdkr_user_video_config_path(char *output, size_t output_size);
int mdkr_user_save_directory(char *output, size_t output_size);
int mdkr_user_resource_path(const char *relative_path,
                            char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_USER_PATHS_H */
