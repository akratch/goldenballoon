#include "taj_mod_state_file.h"
#include "magic_codes_state_file.h"
#include "fs_utf8.h"
#include "test_platform_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* user_paths.c keeps SDL out of its public contract; satisfy its packaged-path
 * provider even though this focused test uses the explicit save override. */
char *SDL_GetPrefPath(const char *organization, const char *application) {
    (void)organization;
    (void)application;
    return NULL;
}

void SDL_free(void *memory) { free(memory); }

int main(void) {
    char root[1024];
    char save[1200];
    char path[1400];
    char temporary[1400];
    TajModPersistentState expected;
    TajModPersistentState loaded;
    MagicCodesPersistentState magic_expected;
    MagicCodesPersistentState magic_loaded;
    const TajModStateStorage *storage = taj_mod_state_file_storage();
    const MagicCodesStateStorage *magic_storage =
        magic_codes_state_file_storage();
    int written;
    int failed = 0;
    int path_exists = 0;
    int temporary_exists = 0;
    FILE *sentinel;
    char sentinel_text[16] = {0};

    if (!mdkr_test_make_temp_directory(root, sizeof(root), "mdkr-taj-state")) {
        fprintf(stderr, "FAIL: could not create temporary root\n");
        return 1;
    }
    written = snprintf(save, sizeof(save), "%s/new-save-directory", root);
    if (written < 0 || (size_t)written >= sizeof(save) ||
        mdkr_test_env_set("MDKR_SAVE_DIR", save, 1) != 0) {
        fprintf(stderr, "FAIL: could not configure isolated save directory\n");
        return 1;
    }

    taj_mod_state_defaults(&expected);
    expected.taj_unlocked = 1;
    expected.adventure_migration_complete = 1;
    if (taj_mod_state_store(&expected, storage) != 1) {
        fprintf(stderr, "FAIL: store did not create a missing save directory\n");
        failed = 1;
    }
    taj_mod_state_defaults(&loaded);
    if (taj_mod_state_load(&loaded, storage) != 1 ||
        loaded.taj_unlocked != 1 ||
        loaded.adventure_migration_complete != 1) {
        fprintf(stderr, "FAIL: stored sidecar did not load exactly\n");
        failed = 1;
    }

    magic_codes_state_defaults(&magic_expected);
    magic_expected.unlocked =
        (UINT32_C(1) << 5) | (UINT32_C(1) << 25);
    magic_expected.active = magic_expected.unlocked;
    if (magic_codes_state_store(&magic_expected, magic_storage) != 1) {
        fprintf(stderr, "FAIL: Magic Code store failed\n");
        failed = 1;
    }
    magic_codes_state_defaults(&magic_loaded);
    if (magic_codes_state_load(&magic_loaded, magic_storage) != 1 ||
        magic_loaded.unlocked != magic_expected.unlocked ||
        magic_loaded.active != magic_expected.active) {
        fprintf(stderr, "FAIL: Magic Code sidecar did not round-trip exactly\n");
        failed = 1;
    }
    (void)snprintf(path, sizeof(path), "%s/magic_codes_state.ini", save);
    (void)mdkr_path_query_utf8(path, &path_exists, NULL, NULL);
    if (!path_exists) {
        fprintf(stderr, "FAIL: Magic Code sidecar used the wrong path\n");
        failed = 1;
    }

    (void)snprintf(path, sizeof(path), "%s/taj_mod_state.ini", save);
    (void)snprintf(temporary, sizeof(temporary), "%s/taj_mod_state.ini.tmp", save);
    (void)mdkr_path_query_utf8(path, &path_exists, NULL, NULL);
    (void)mdkr_path_query_utf8(temporary, &temporary_exists, NULL, NULL);
    if (!path_exists || temporary_exists) {
        fprintf(stderr, "FAIL: atomic install left the wrong files\n");
        failed = 1;
    }

    /* A stale staging file from an older build or another process must never
     * be truncated, reused, or prevent a new atomic save. */
    sentinel = mdkr_fopen_utf8(temporary, "wb");
    if (sentinel == NULL) {
        fprintf(stderr, "FAIL: could not create staging-file sentinel\n");
        failed = 1;
    } else {
        int sentinel_failed = fwrite("do-not-touch", 1, 12, sentinel) != 12;
        if (fclose(sentinel) != 0) sentinel_failed = 1;
        if (sentinel_failed) {
            fprintf(stderr, "FAIL: could not write staging-file sentinel\n");
            failed = 1;
        }
        expected.adventure_migration_complete = 0;
        if (taj_mod_state_store(&expected, storage) != 1) {
            fprintf(stderr, "FAIL: stale staging file blocked a save\n");
            failed = 1;
        }
        sentinel = mdkr_fopen_utf8(temporary, "rb");
        if (sentinel == NULL) {
            fprintf(stderr, "FAIL: save reused another transaction's staging file\n");
            failed = 1;
        } else {
            int sentinel_failed = fread(sentinel_text, 1, 12, sentinel) != 12;
            if (fclose(sentinel) != 0) sentinel_failed = 1;
            if (sentinel_failed || strcmp(sentinel_text, "do-not-touch") != 0) {
                fprintf(stderr, "FAIL: save reused another transaction's staging file\n");
                failed = 1;
            }
        }
        taj_mod_state_defaults(&loaded);
        if (taj_mod_state_load(&loaded, storage) != 1 ||
            loaded.taj_unlocked != 1 ||
            loaded.adventure_migration_complete != 0) {
            fprintf(stderr, "FAIL: save beside stale staging file was not installed\n");
            failed = 1;
        }
    }

    (void)mdkr_test_env_unset("MDKR_SAVE_DIR");
    (void)mdkr_remove_utf8(temporary);
    (void)snprintf(temporary, sizeof(temporary), "%s/taj_mod_state.ini.lock", save);
    (void)mdkr_remove_utf8(temporary);
    (void)mdkr_remove_utf8(path);
    (void)snprintf(path, sizeof(path), "%s/magic_codes_state.ini", save);
    (void)mdkr_remove_utf8(path);
    (void)snprintf(path, sizeof(path), "%s/magic_codes_state.ini.lock", save);
    (void)mdkr_remove_utf8(path);
    (void)mdkr_rmdir_utf8(save);
    (void)mdkr_rmdir_utf8(root);
    return failed;
}
