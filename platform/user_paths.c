#include "user_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include <unistd.h>
#elif defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define MDKR_USER_PATH_MAX 4096
#define MDKR_PACKAGE_MARKER ".app/Contents/MacOS/"

#ifndef __EMSCRIPTEN__
/* Keep this C policy module independent of SDL headers so its packaged-path
 * unit test can provide a tiny deterministic link-time preference provider. */
extern char *SDL_GetPrefPath(const char *org, const char *app);
extern void SDL_free(void *memory);
#endif

static int s_packaged;
static int s_pref_ready;
static char s_resource_dir[MDKR_USER_PATH_MAX];
static char s_pref_dir[MDKR_USER_PATH_MAX];
static char s_launch_cwd[MDKR_USER_PATH_MAX];

static int path_copy(char *output, size_t output_size, const char *value) {
    size_t length;
    if (output == NULL || output_size == 0u || value == NULL) {
        return 0;
    }
    length = strlen(value);
    if (length >= output_size) {
        return 0;
    }
    memcpy(output, value, length + 1u);
    return 1;
}

static int path_join(char *output, size_t output_size,
                     const char *directory, const char *leaf) {
    size_t directory_length;
    const char *separator;
    int written;
    if (output == NULL || output_size == 0u || directory == NULL ||
        leaf == NULL || directory[0] == '\0') {
        return 0;
    }
    directory_length = strlen(directory);
    separator = (directory[directory_length - 1u] == '/' ||
                 directory[directory_length - 1u] == '\\') ? "" : "/";
    written = snprintf(output, output_size, "%s%s%s",
                       directory, separator, leaf);
    return written >= 0 && (size_t)written < output_size;
}

static int path_parent(char *output, size_t output_size, const char *path) {
    const char *slash;
    const char *backslash;
    const char *last;
    size_t length;
    if (path == NULL) {
        return 0;
    }
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    last = slash;
    if (backslash != NULL && (last == NULL || backslash > last)) {
        last = backslash;
    }
    if (last == NULL) {
        return path_copy(output, output_size, ".");
    }
    length = (size_t)(last - path);
    if (length == 0u) {
        length = 1u;
    }
    if (length >= output_size) {
        return 0;
    }
    memcpy(output, path, length);
    output[length] = '\0';
    return 1;
}

static int path_is_directory(const char *path) {
    struct stat status;
    return path != NULL && stat(path, &status) == 0 &&
           S_ISDIR(status.st_mode);
}

static int path_is_regular(const char *path) {
    struct stat status;
#if !defined(_WIN32)
    if (path == NULL || lstat(path, &status) != 0) {
        return 0;
    }
#else
    if (path == NULL || stat(path, &status) != 0) {
        return 0;
    }
#endif
    return S_ISREG(status.st_mode);
}

static int path_exists(const char *path) {
    struct stat status;
    return path != NULL && stat(path, &status) == 0;
}

static int make_private_directory(const char *path) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int sync_file(FILE *file) {
#if defined(__EMSCRIPTEN__)
    (void)file;
    return 0;
#elif defined(_WIN32)
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

static void sync_directory_best_effort(const char *path) {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    int descriptor = open(path, O_RDONLY | O_DIRECTORY);
    if (descriptor >= 0) {
        (void)fsync(descriptor);
        (void)close(descriptor);
    }
#else
    (void)path;
#endif
}

static long process_id(void) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int copy_regular_file(const char *source, const char *destination) {
    unsigned char buffer[16384];
    FILE *input;
    FILE *output;
    int failed = 0;

    if (!path_is_regular(source)) {
        return 0;
    }
    input = fopen(source, "rb");
    if (input == NULL) {
        return -1;
    }
    /* Every migration destination is a private staging name. Exclusive create
     * prevents a stale file or same-user concurrent launch from being
     * truncated between the existence check and fopen. */
    output = fopen(destination, "wbx");
    if (output == NULL) {
        (void)fclose(input);
        return -1;
    }
    for (;;) {
        size_t count = fread(buffer, 1u, sizeof(buffer), input);
        if (count != 0u && fwrite(buffer, 1u, count, output) != count) {
            failed = 1;
            break;
        }
        if (count < sizeof(buffer)) {
            if (ferror(input)) {
                failed = 1;
            }
            break;
        }
    }
    if (fflush(output) != 0 || sync_file(output) != 0) {
        failed = 1;
    }
    if (fclose(input) != 0) {
        failed = 1;
    }
    if (fclose(output) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)remove(destination);
        return -1;
    }
    return 1;
}

/* Install a staged file without replacing a destination created concurrently. */
static int install_file_exclusive(const char *staged, const char *destination) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return MoveFileExA(staged, destination, MOVEFILE_WRITE_THROUGH) ? 1 : 0;
#else
    if (link(staged, destination) != 0) {
        return 0;
    }
    (void)unlink(staged);
    return 1;
#endif
}

static int install_directory_exclusive(const char *staged,
                                       const char *destination) {
    if (path_exists(destination)) {
        return 0;
    }
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return MoveFileExA(staged, destination, MOVEFILE_WRITE_THROUGH) ? 1 : 0;
#elif defined(__APPLE__)
    return renamex_np(staged, destination, RENAME_EXCL) == 0;
#else
    /* Non-packaged Linux builds never enter migration. Keep the portable
     * fallback fail-safe by rechecking immediately before the same-filesystem
     * directory rename. */
    return !path_exists(destination) && rename(staged, destination) == 0;
#endif
}

static int relative_from_root(char *output, size_t output_size,
                              const char *root, const char *relative) {
    return root != NULL && root[0] != '\0' &&
           path_join(output, output_size, root, relative);
}

static int legacy_candidate(char *output, size_t output_size,
                            const char *relative, int cwd_candidate) {
    const char *root = cwd_candidate ? s_launch_cwd : s_resource_dir;
    return relative_from_root(output, output_size, root, relative);
}

static int migrate_video_config(void) {
    char destination[MDKR_USER_PATH_MAX];
    char destination_parent[MDKR_USER_PATH_MAX];
    char source[MDKR_USER_PATH_MAX];
    char staged[MDKR_USER_PATH_MAX];
    int source_found = 0;
    int written;

    const char *override = getenv("MDKR_VIDEO_CONFIG_PATH");
    staged[0] = '\0';
    if (override != NULL && override[0] != '\0') {
        return 1;
    }
    if (!mdkr_user_video_config_path(destination, sizeof(destination))) {
        return 0;
    }
    if (path_exists(destination)) {
        return 1;
    }
    /* Resources is first because the prior packaged launcher made it CWD. */
    if (legacy_candidate(source, sizeof(source), "mdkr64.ini", 0) &&
        path_is_regular(source)) {
        source_found = 1;
    } else if (strcmp(s_launch_cwd, s_resource_dir) != 0 &&
               legacy_candidate(source, sizeof(source), "mdkr64.ini", 1) &&
               path_is_regular(source)) {
        source_found = 1;
    }
    if (!source_found) {
        return 1;
    }
    written = snprintf(staged, sizeof(staged), "%s.migrate-%ld.tmp",
                       destination, process_id());
    if (written < 0 || (size_t)written >= sizeof(staged) ||
        path_exists(staged) || copy_regular_file(source, staged) != 1) {
        fprintf(stderr, "[paths] could not stage legacy video config from %s\n",
                source);
        if (staged[0] != '\0') {
            (void)remove(staged);
        }
        return 0;
    }
    if (!install_file_exclusive(staged, destination)) {
        /* Another process may have installed a destination after our first
         * check. Its file wins; the legacy source remains untouched. */
        (void)remove(staged);
        if (path_exists(destination)) {
            return 1;
        }
        fprintf(stderr, "[paths] could not install migrated video config %s\n",
                destination);
        return 0;
    }
    if (path_parent(destination_parent, sizeof(destination_parent),
                    destination)) {
        sync_directory_best_effort(destination_parent);
    }
    fprintf(stderr, "[paths] copied legacy video config to %s\n", destination);
    return 1;
}

static int save_name(char *output, size_t output_size,
                     unsigned category, unsigned first, unsigned second) {
    int written;
    switch (category) {
        case 0:
            return path_copy(output, output_size, "eeprom.bin");
        case 1:
            return path_copy(output, output_size, "eeprom.bin.bad");
        case 2:
            return path_copy(output, output_size, "eeprom.bin.previous");
        case 3:
            return path_copy(output, output_size, "eeprom.bin.importing");
        case 4:
            written = snprintf(output, output_size,
                               "eeprom.bin.autosave.%u", first);
            break;
        case 5:
            written = snprintf(output, output_size,
                               "controller-pak-%u.mdp", first);
            break;
        default:
            written = snprintf(output, output_size,
                               "controller-pak-%u.mdp.bad.%u", first, second);
            break;
    }
    return written >= 0 && (size_t)written < output_size;
}

typedef int (*SaveNameVisitor)(const char *name, void *context);

static int visit_save_names(SaveNameVisitor visitor, void *context) {
    char name[128];
    unsigned category;
    unsigned first;
    unsigned second;
    for (category = 0u; category <= 3u; category++) {
        if (!save_name(name, sizeof(name), category, 0u, 0u) ||
            !visitor(name, context)) {
            return 0;
        }
    }
    for (first = 1u; first <= 3u; first++) {
        if (!save_name(name, sizeof(name), 4u, first, 0u) ||
            !visitor(name, context)) {
            return 0;
        }
    }
    for (first = 1u; first <= 4u; first++) {
        if (!save_name(name, sizeof(name), 5u, first, 0u) ||
            !visitor(name, context)) {
            return 0;
        }
        for (second = 1u; second <= 99u; second++) {
            if (!save_name(name, sizeof(name), 6u, first, second) ||
                !visitor(name, context)) {
                return 0;
            }
        }
    }
    return 1;
}

typedef struct SaveSourceProbe {
    const char *directory;
    int found;
} SaveSourceProbe;

static int probe_save_source(const char *name, void *opaque) {
    SaveSourceProbe *probe = (SaveSourceProbe *)opaque;
    char path[MDKR_USER_PATH_MAX];
    if (!probe->found && path_join(path, sizeof(path), probe->directory, name) &&
        path_is_regular(path)) {
        probe->found = 1;
    }
    return 1;
}

typedef struct SaveCopyContext {
    const char *source;
    const char *staged;
    int copied;
    int failed;
} SaveCopyContext;

static int copy_save_entry(const char *name, void *opaque) {
    SaveCopyContext *context = (SaveCopyContext *)opaque;
    char source[MDKR_USER_PATH_MAX];
    char destination[MDKR_USER_PATH_MAX];
    int result;
    if (!path_join(source, sizeof(source), context->source, name) ||
        !path_join(destination, sizeof(destination), context->staged, name)) {
        context->failed = 1;
        return 0;
    }
    result = copy_regular_file(source, destination);
    if (result < 0) {
        context->failed = 1;
        return 0;
    }
    if (result > 0) {
        context->copied++;
    }
    return 1;
}

typedef struct SaveCleanupContext {
    const char *staged;
} SaveCleanupContext;

static int cleanup_save_entry(const char *name, void *opaque) {
    SaveCleanupContext *context = (SaveCleanupContext *)opaque;
    char path[MDKR_USER_PATH_MAX];
    if (path_join(path, sizeof(path), context->staged, name)) {
        (void)remove(path);
    }
    return 1;
}

static void cleanup_staged_save(const char *staged) {
    SaveCleanupContext context = { staged };
    (void)visit_save_names(cleanup_save_entry, &context);
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    (void)_rmdir(staged);
#else
    (void)rmdir(staged);
#endif
}

static int source_save_directory(char *output, size_t output_size) {
    char candidate[MDKR_USER_PATH_MAX];
    SaveSourceProbe probe;
    if (legacy_candidate(candidate, sizeof(candidate), "save", 0) &&
        path_is_directory(candidate)) {
        probe.directory = candidate;
        probe.found = 0;
        (void)visit_save_names(probe_save_source, &probe);
        if (probe.found) {
            return path_copy(output, output_size, candidate);
        }
    }
    if (strcmp(s_launch_cwd, s_resource_dir) != 0 &&
        legacy_candidate(candidate, sizeof(candidate), "save", 1) &&
        path_is_directory(candidate)) {
        probe.directory = candidate;
        probe.found = 0;
        (void)visit_save_names(probe_save_source, &probe);
        if (probe.found) {
            return path_copy(output, output_size, candidate);
        }
    }
    return 0;
}

static int migrate_save_directory(void) {
    char destination[MDKR_USER_PATH_MAX];
    char destination_parent[MDKR_USER_PATH_MAX];
    char source[MDKR_USER_PATH_MAX];
    char staged[MDKR_USER_PATH_MAX];
    SaveCopyContext context;
    int written;

    const char *override = getenv("MDKR_SAVE_DIR");
    staged[0] = '\0';
    if (override != NULL && override[0] != '\0') {
        return 1;
    }
    if (!mdkr_user_save_directory(destination, sizeof(destination))) {
        return 0;
    }
    if (path_exists(destination)) {
        return 1;
    }
    if (!source_save_directory(source, sizeof(source))) {
        return 1;
    }
    written = snprintf(staged, sizeof(staged), "%s.migrate-%ld.tmp",
                       destination, process_id());
    if (written < 0 || (size_t)written >= sizeof(staged) ||
        path_exists(staged) || make_private_directory(staged) != 0) {
        fprintf(stderr, "[paths] could not create staged save migration %s\n",
                staged);
        return 0;
    }
    context.source = source;
    context.staged = staged;
    context.copied = 0;
    context.failed = 0;
    if (!visit_save_names(copy_save_entry, &context) || context.failed ||
        context.copied == 0) {
        cleanup_staged_save(staged);
        fprintf(stderr, "[paths] could not stage legacy save data from %s\n",
                source);
        return 0;
    }
    sync_directory_best_effort(staged);
    if (!install_directory_exclusive(staged, destination)) {
        cleanup_staged_save(staged);
        if (path_exists(destination)) {
            return 1;
        }
        fprintf(stderr, "[paths] could not install migrated save directory %s\n",
                destination);
        return 0;
    }
    if (path_parent(destination_parent, sizeof(destination_parent),
                    destination)) {
        sync_directory_best_effort(destination_parent);
    }
    fprintf(stderr, "[paths] copied %d legacy save file(s) to %s\n",
            context.copied, destination);
    return 1;
}

int mdkr_user_paths_init(const char *executable_path) {
#ifdef __EMSCRIPTEN__
    (void)executable_path;
    return 0;
#else
    const char *marker;
    char *preference_path;
    size_t resource_prefix;
    int written;

    if (s_packaged) {
        return mdkr_user_paths_prepare_packaged_data() ? 1 : -1;
    }
    if (executable_path == NULL ||
        (marker = strstr(executable_path, MDKR_PACKAGE_MARKER)) == NULL) {
        return 0;
    }
    s_packaged = 1;
    resource_prefix = (size_t)(marker - executable_path) + sizeof(".app") - 1u;
    written = snprintf(s_resource_dir, sizeof(s_resource_dir), "%.*s/Contents/Resources",
                       (int)resource_prefix, executable_path);
    if (written < 0 || (size_t)written >= sizeof(s_resource_dir)) {
        fprintf(stderr, "[paths] packaged Resources path is too long\n");
        return -1;
    }
#if defined(_WIN32)
    if (_getcwd(s_launch_cwd, sizeof(s_launch_cwd)) == NULL) {
#else
    if (getcwd(s_launch_cwd, sizeof(s_launch_cwd)) == NULL) {
#endif
        fprintf(stderr, "[paths] could not capture launch directory: %s\n",
                strerror(errno));
        return -1;
    }
    preference_path = SDL_GetPrefPath("mdkr64", "mdkr64");
    if (preference_path == NULL || preference_path[0] == '\0' ||
        !path_copy(s_pref_dir, sizeof(s_pref_dir), preference_path)) {
        fprintf(stderr,
                "[paths] packaged app requires a writable per-user directory; "
                "SDL_GetPrefPath(mdkr64, mdkr64) failed\n");
        if (preference_path != NULL) {
            SDL_free(preference_path);
        }
        return -1;
    }
    SDL_free(preference_path);
    s_pref_ready = 1;
    if (!mdkr_user_paths_prepare_packaged_data()) {
        return -1;
    }
    return 1;
#endif
}

int mdkr_user_paths_prepare_packaged_data(void) {
#ifdef __EMSCRIPTEN__
    return 1;
#else
    if (!s_packaged) {
        return 1;
    }
    if (!s_pref_ready) {
        return 0;
    }
    return migrate_video_config() && migrate_save_directory();
#endif
}

int mdkr_user_paths_is_packaged(void) {
    return s_packaged;
}

int mdkr_user_video_config_path(char *output, size_t output_size) {
#ifdef __EMSCRIPTEN__
    return path_copy(output, output_size, "/save/mdkr64.ini");
#else
    const char *override = getenv("MDKR_VIDEO_CONFIG_PATH");
    if (override != NULL && override[0] != '\0') {
        return path_copy(output, output_size, override);
    }
    if (s_packaged) {
        return s_pref_ready &&
               path_join(output, output_size, s_pref_dir, "mdkr64.ini");
    }
    return path_copy(output, output_size, "mdkr64.ini");
#endif
}

int mdkr_user_save_directory(char *output, size_t output_size) {
#ifdef __EMSCRIPTEN__
    return path_copy(output, output_size, "/save");
#else
    const char *override = getenv("MDKR_SAVE_DIR");
    if (override != NULL && override[0] != '\0') {
        return path_copy(output, output_size, override);
    }
    if (s_packaged) {
        return s_pref_ready && path_join(output, output_size, s_pref_dir, "save");
    }
    return path_copy(output, output_size, "save");
#endif
}

int mdkr_user_resource_path(const char *relative_path,
                            char *output, size_t output_size) {
    if (relative_path == NULL || relative_path[0] == '\0') {
        return 0;
    }
    if (s_packaged) {
        return path_join(output, output_size, s_resource_dir, relative_path);
    }
    return path_copy(output, output_size, relative_path);
}
