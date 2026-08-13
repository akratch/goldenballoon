/*
 * Unit coverage for the issue #33 portable / write-fallback path policy in
 * platform/user_paths.c.
 *
 * Two independent process modes keep the lazy portable-mode detection (which
 * caches once per process) from having to be re-armed inside a single run:
 *
 *   default     A portable.txt is created beside THIS test executable before any
 *               path query. Config, save, and mods must then resolve next to the
 *               executable, and an environment override must still win over it.
 *
 *   --fallback  No portable.txt. The getters return their historical
 *               CWD-relative defaults until a simulated home-directory write
 *               failure activates the fallback, after which every path resolves
 *               next to the executable and the relocation notice latches.
 *
 * The two modes never share on-disk state: only the default mode ever creates
 * portable.txt, and it removes it before exiting, so a parallel --fallback run
 * cannot observe it. The executable directory is derived here independently of
 * user_paths.c so the assertions cannot pass by echoing the code under test.
 */
#include "user_paths.h"
#include "fs_utf8.h"
#include "test_platform_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Link-time SDL preference provider so user_paths.c resolves without SDL. The
 * portable and fallback code paths deliberately never reach it. */
char *SDL_GetPrefPath(const char *organization, const char *application) {
    static const char kPref[] = "/tmp/mdkr-portable-unused-pref/";
    char *result;
    (void)organization;
    (void)application;
    result = (char *)malloc(sizeof(kPref));
    if (result != NULL) {
        memcpy(result, kPref, sizeof(kPref));
    }
    return result;
}

void SDL_free(void *memory) { free(memory); }

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

/* Independent parent-directory derivation, handling both separators. */
static int parent_directory(char *output, size_t size, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last = slash;
    size_t length;
    if (backslash != NULL && (last == NULL || backslash > last)) {
        last = backslash;
    }
    if (last == NULL) {
        return 0;
    }
    length = (size_t)(last - path);
    if (length == 0u) {
        length = 1u;
    }
    if (length >= size) {
        return 0;
    }
    memcpy(output, path, length);
    output[length] = '\0';
    return 1;
}

static int join(char *output, size_t size, const char *dir, const char *leaf) {
    int written = snprintf(output, size, "%s/%s", dir, leaf);
    return written > 0 && (size_t)written < size;
}

static int resolve_exe_dir(char *output, size_t size) {
    char *executable = NULL;
    int ok;
    if (mdkr_running_executable_path_utf8(&executable) != 0 ||
        executable == NULL) {
        return 0;
    }
    ok = parent_directory(output, size, executable);
    free(executable);
    return ok;
}

int main(int argc, char **argv) {
    const int fallback_mode = argc > 1 && strcmp(argv[1], "--fallback") == 0;
    char exe_dir[4096];
    char expected[4096];
    char marker[4096];
    char resolved[4096];
    char override_config[4096];

    /* A stray override inherited from the caller's shell would silence every
     * precedence assertion below; strip both before anything reads them. */
    (void)mdkr_test_env_unset("MDKR_VIDEO_CONFIG_PATH");
    (void)mdkr_test_env_unset("MDKR_SAVE_DIR");

    expect("resolved executable directory",
           resolve_exe_dir(exe_dir, sizeof(exe_dir)));

    if (!fallback_mode) {
        /* Arm portable mode by dropping the marker beside this executable
         * before the first path query triggers detection. */
        expect("portable marker path",
               join(marker, sizeof(marker), exe_dir, "portable.txt"));
        {
            FILE *file = mdkr_fopen_utf8(marker, "wb");
            expect("created portable marker", file != NULL);
            if (file != NULL) {
                (void)fputs("portable\n", file);
                (void)fclose(file);
            }
        }
        expect("portable mode detected", mdkr_user_paths_is_portable());
        expect("config resolves next to the executable",
               mdkr_user_video_config_path(resolved, sizeof(resolved)) &&
               join(expected, sizeof(expected), exe_dir, "mdkr64.ini") &&
               strcmp(resolved, expected) == 0);
        expect("save resolves next to the executable",
               mdkr_user_save_directory(resolved, sizeof(resolved)) &&
               join(expected, sizeof(expected), exe_dir, "save") &&
               strcmp(resolved, expected) == 0);
        expect("mods resolves next to the executable",
               mdkr_user_mods_directory(resolved, sizeof(resolved)) &&
               join(expected, sizeof(expected), exe_dir, "mods") &&
               strcmp(resolved, expected) == 0);
        /* A portable install is never a write-relocation, so it must not raise
         * the "saved next to the game" notice. */
        expect("portable mode is not a relocation notice",
               !mdkr_user_paths_write_relocated());
        /* Environment overrides still win over portable mode (the suite depends
         * on MDKR_VIDEO_CONFIG_PATH selecting an isolated file). */
        expect("override path",
               join(override_config, sizeof(override_config),
                    exe_dir, "override.ini"));
        expect("set video override",
               mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH",
                                 override_config, 1) == 0);
        expect("env override beats portable mode",
               mdkr_user_video_config_path(resolved, sizeof(resolved)) &&
               strcmp(resolved, override_config) == 0);
        (void)mdkr_test_env_unset("MDKR_VIDEO_CONFIG_PATH");
        (void)mdkr_remove_utf8(marker);
    } else {
        /* No marker: the getters keep their historical CWD-relative spellings
         * until the fallback is activated. */
        expect("not portable without a marker", !mdkr_user_paths_is_portable());
        expect("config is CWD-relative before fallback",
               mdkr_user_video_config_path(resolved, sizeof(resolved)) &&
               strcmp(resolved, "mdkr64.ini") == 0);
        expect("save is CWD-relative before fallback",
               mdkr_user_save_directory(resolved, sizeof(resolved)) &&
               strcmp(resolved, "save") == 0);
        expect("no relocation notice before a failure",
               !mdkr_user_paths_write_relocated());
        /* Simulate the home-directory write failing. */
        expect("fallback activation reports a new location",
               mdkr_user_paths_activate_write_fallback());
        expect("relocation notice latched",
               mdkr_user_paths_write_relocated());
        expect("config relocates next to the executable",
               mdkr_user_video_config_path(resolved, sizeof(resolved)) &&
               join(expected, sizeof(expected), exe_dir, "mdkr64.ini") &&
               strcmp(resolved, expected) == 0);
        expect("save relocates next to the executable",
               mdkr_user_save_directory(resolved, sizeof(resolved)) &&
               join(expected, sizeof(expected), exe_dir, "save") &&
               strcmp(resolved, expected) == 0);
        expect("mods relocates next to the executable",
               mdkr_user_mods_directory(resolved, sizeof(resolved)) &&
               join(expected, sizeof(expected), exe_dir, "mods") &&
               strcmp(resolved, expected) == 0);
        /* A second activation offers nothing new, so it must report 0 and not
         * leave the caller retrying forever. */
        expect("second fallback activation is a no-op",
               !mdkr_user_paths_activate_write_fallback());
        /* Even relocated, an explicit override still wins. */
        expect("override path",
               join(override_config, sizeof(override_config),
                    exe_dir, "override.ini"));
        expect("set video override",
               mdkr_test_env_set("MDKR_VIDEO_CONFIG_PATH",
                                 override_config, 1) == 0);
        expect("env override beats the fallback",
               mdkr_user_video_config_path(resolved, sizeof(resolved)) &&
               strcmp(resolved, override_config) == 0);
        (void)mdkr_test_env_unset("MDKR_VIDEO_CONFIG_PATH");
    }

    if (s_failures != 0) {
        fprintf(stderr, "%d portable-path test(s) failed\n", s_failures);
        return 1;
    }
    puts(fallback_mode ? "portable write fallback: PASS"
                       : "portable marker: PASS");
    return 0;
}
