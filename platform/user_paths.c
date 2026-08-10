#include "user_paths.h"
#include "fs_utf8.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include <unistd.h>
#elif defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define MDKR_USER_PATH_MAX 4096
#define MDKR_PACKAGE_MARKER ".app/Contents/MacOS/"
#define MDKR_MIGRATION_OWNER ".mdkr64-migration-owner"
#define MDKR_MIGRATION_OWNER_MAGIC "mdkr64-save-migration-v1\n"

/* Packaged desktop migration is deliberately compiled into the shared web
 * target so this policy remains one implementation. Emscripten uses fixed
 * /save paths and never calls the two migration roots, so mark only those
 * roots (and their desktop preference storage) as intentionally dormant. */
#ifdef __EMSCRIPTEN__
#define MDKR_PACKAGED_ONLY __attribute__((unused))
#else
#define MDKR_PACKAGED_ONLY
#endif

#ifndef __EMSCRIPTEN__
/* Keep this C policy module independent of SDL headers so its packaged-path
 * unit test can provide a tiny deterministic link-time preference provider. */
extern char *SDL_GetPrefPath(const char *org, const char *app);
extern void SDL_free(void *memory);
#endif

static int s_packaged;
static int s_pref_ready MDKR_PACKAGED_ONLY;
static char s_resource_dir[MDKR_USER_PATH_MAX];
static char s_pref_dir[MDKR_USER_PATH_MAX] MDKR_PACKAGED_ONLY;
static char s_launch_cwd[MDKR_USER_PATH_MAX];
static char s_last_error[MDKR_USER_PATH_MAX + 512];

static void set_last_error(const char *message, const char *path) {
    if (message == NULL) {
        s_last_error[0] = '\0';
        return;
    }
    (void)snprintf(s_last_error, sizeof(s_last_error),
                   path != NULL ? "%s: %s" : "%s", message, path);
}

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
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    int exists = 0;
    int directory = 0;
    mdkr_path_query_utf8(path, &exists, NULL, &directory);
    return exists && directory;
#else
    struct stat status;
    return path != NULL && stat(path, &status) == 0 &&
           S_ISDIR(status.st_mode);
#endif
}

/* Migration cleanup only ever owns a directory it created. Do not follow a
 * link/reparse point here: cleanup_staged_save() addresses known child names,
 * which would otherwise unlink files in an attacker-controlled target. */
static int path_is_owned_stage_directory(const char *path) {
    return path_is_directory(path) &&
           mdkr_path_is_link_or_reparse_utf8(path) == 0;
}

static int path_is_regular(const char *path) {
#if !defined(_WIN32)
    struct stat status;
    if (path == NULL || lstat(path, &status) != 0) {
        return 0;
    }
#else
    int exists = 0;
    int regular = 0;
    mdkr_path_query_utf8(path, &exists, &regular, NULL);
    return exists && regular;
#endif
#if !defined(_WIN32)
    return S_ISREG(status.st_mode);
#endif
}

static int path_exists(const char *path) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    int exists = 0;
    mdkr_path_query_utf8(path, &exists, NULL, NULL);
    return exists;
#else
    struct stat status;
    return path != NULL && stat(path, &status) == 0;
#endif
}

static int make_private_directory(const char *path) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return mdkr_mkdir_utf8(path);
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
    input = mdkr_fopen_utf8(source, "rb");
    if (input == NULL) {
        return -1;
    }
    /* Every migration destination is a private staging name. Exclusive create
     * prevents a stale file or same-user concurrent launch from being
     * truncated between the existence check and fopen. */
    output = mdkr_fopen_utf8(destination, "wbx");
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
        (void)mdkr_remove_utf8(destination);
        return -1;
    }
    return 1;
}

/* Install a staged file without replacing a destination created concurrently. */
static int install_file_exclusive(const char *staged, const char *destination) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return mdkr_move_utf8(staged, destination, 0, 1) == 0 ? 1 : 0;
#else
    if (link(staged, destination) != 0) {
        return 0;
    }
    (void)mdkr_unlink_utf8(staged);
    return 1;
#endif
}

static int install_directory_exclusive(const char *staged,
                                       const char *destination) {
    if (path_exists(destination)) {
        return 0;
    }
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return mdkr_move_utf8(staged, destination, 0, 1) == 0 ? 1 : 0;
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

static int MDKR_PACKAGED_ONLY migrate_video_config(void) {
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
            (void)mdkr_remove_utf8(staged);
        }
        return 0;
    }
    if (!install_file_exclusive(staged, destination)) {
        /* Another process may have installed a destination after our first
         * check. Its file wins; the legacy source remains untouched. */
        (void)mdkr_remove_utf8(staged);
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
        (void)mdkr_remove_utf8(path);
    }
    return 1;
}

static void cleanup_staged_save(const char *staged) {
    SaveCleanupContext context = { staged };
    char owner[MDKR_USER_PATH_MAX];
    (void)visit_save_names(cleanup_save_entry, &context);
    if (path_join(owner, sizeof(owner), staged, MDKR_MIGRATION_OWNER)) {
        (void)mdkr_remove_utf8(owner);
    }
    (void)mdkr_rmdir_utf8(staged);
}

static int migration_owner_text(char *output, size_t output_size,
                                const char *destination) {
    int written = snprintf(output, output_size, "%s%s",
                           MDKR_MIGRATION_OWNER_MAGIC, destination);
    return written >= 0 && (size_t)written < output_size;
}

static int write_migration_owner(const char *staged,
                                 const char *destination) {
    char path[MDKR_USER_PATH_MAX];
    char text[MDKR_USER_PATH_MAX + 64];
    FILE *file;
    size_t size;
    int good;
    if (!path_join(path, sizeof(path), staged, MDKR_MIGRATION_OWNER) ||
        !migration_owner_text(text, sizeof(text), destination)) {
        return 0;
    }
    file = mdkr_fopen_utf8(path, "wbx");
    if (file == NULL) {
        return 0;
    }
    size = strlen(text);
    good = fwrite(text, 1u, size, file) == size;
    if (fflush(file) != 0 || sync_file(file) != 0) {
        good = 0;
    }
    if (fclose(file) != 0) {
        good = 0;
    }
    if (!good) {
        (void)mdkr_remove_utf8(path);
    }
    return good;
}

static int migration_owner_matches(const char *directory,
                                   const char *destination) {
    char path[MDKR_USER_PATH_MAX];
    char expected[MDKR_USER_PATH_MAX + 64];
    char actual[MDKR_USER_PATH_MAX + 64];
    FILE *file;
    size_t expected_size;
    size_t actual_size;
    int trailing;
    if (!path_join(path, sizeof(path), directory, MDKR_MIGRATION_OWNER) ||
        !path_is_regular(path) ||
        !migration_owner_text(expected, sizeof(expected), destination)) {
        return 0;
    }
    file = mdkr_fopen_utf8(path, "rb");
    if (file == NULL) {
        return 0;
    }
    expected_size = strlen(expected);
    actual_size = fread(actual, 1u, sizeof(actual), file);
    trailing = fgetc(file);
    (void)fclose(file);
    return actual_size == expected_size && trailing == EOF &&
           memcmp(actual, expected, expected_size) == 0;
}

/*
 * Clear THIS launch's stage path if a dead process left it behind.
 *
 * `staged` is always the caller's own PID-derived name, so this is a
 * name-collision guard, not a garbage collector: it fires when the OS hands
 * this process a PID whose previous owner was interrupted mid-migration, which
 * is exactly the case that would otherwise fail mkdir() with no explanation.
 * Stages left by other PIDs are none of this launch's business and are left
 * untouched (docs/APP_SHELL.md says so too). Returns 1 when the path is free to
 * create, 0 when it holds something this process must not touch.
 */
static int recover_abandoned_save_stage(const char *staged,
                                        const char *destination) {
    /* Test the directory entry itself before path_exists(): POSIX stat() and
     * the ordinary Windows directory query follow a link, so a dangling stage
     * would otherwise look absent and later fail mkdir() without the recovery
     * diagnosis. */
    if (mdkr_path_is_link_or_reparse_utf8(staged) == 1) {
        set_last_error(
            "An interrupted save migration could not be verified. Move this "
            "folder aside (or remove it after preserving any files), then "
            "reopen Golden Balloon",
            staged);
        fprintf(stderr, "[paths] %s\n", s_last_error);
        return 0;
    }
    if (!path_exists(staged)) {
        return 1;
    }
    if (!path_is_owned_stage_directory(staged) ||
        !migration_owner_matches(staged, destination)) {
        set_last_error(
            "An interrupted save migration could not be verified. Move this "
            "folder aside (or remove it after preserving any files), then "
            "reopen Golden Balloon",
            staged);
        fprintf(stderr, "[paths] %s\n", s_last_error);
        return 0;
    }
    cleanup_staged_save(staged);
    if (path_exists(staged)) {
        set_last_error(
            "An interrupted save migration contains unexpected files. Move "
            "this folder aside, then reopen Golden Balloon",
            staged);
        fprintf(stderr, "[paths] %s\n", s_last_error);
        return 0;
    }
    fprintf(stderr, "[paths] recovered abandoned save migration %s\n", staged);
    return 1;
}

static void remove_installed_migration_owner(const char *destination) {
    char owner[MDKR_USER_PATH_MAX];
    if (!migration_owner_matches(destination, destination) ||
        !path_join(owner, sizeof(owner), destination, MDKR_MIGRATION_OWNER)) {
        return;
    }
    if (mdkr_remove_utf8(owner) != 0) {
        fprintf(stderr,
                "[paths] warning: could not remove completed migration marker %s\n",
                owner);
    } else {
        sync_directory_best_effort(destination);
    }
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

static int MDKR_PACKAGED_ONLY migrate_save_directory(void) {
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
        if (path_is_directory(destination)) {
            remove_installed_migration_owner(destination);
        }
        return 1;
    }
    if (!source_save_directory(source, sizeof(source))) {
        return 1;
    }
    /*
     * The stage name is PID-derived, so recover_abandoned_save_stage() is not a
     * sweep: it can only ever meet leftovers from an interrupted run whose PID
     * this launch reuses. That is deliberate (a concurrent second instance must
     * not be able to delete this one's stage), and docs/APP_SHELL.md states the
     * same scope. What it means here is that any stage this call creates and
     * then abandons is invisible to every later launch, so this function must
     * clean up after itself precisely rather than probabilistically.
     */
    written = snprintf(staged, sizeof(staged), "%s.migrate-%ld.tmp",
                       destination, process_id());
    if (written < 0 || (size_t)written >= sizeof(staged) ||
        !recover_abandoned_save_stage(staged, destination)) {
        fprintf(stderr, "[paths] could not create staged save migration %s\n",
                staged);
        /* Nothing of ours exists yet: either the name did not fit, or the path
         * is occupied by something this launch has no claim to and must leave
         * exactly as it found it. */
        return 0;
    }
    if (make_private_directory(staged) != 0) {
        fprintf(stderr, "[paths] could not create staged save migration %s\n",
                staged);
        return 0;
    }
    if (!write_migration_owner(staged, destination)) {
        fprintf(stderr, "[paths] could not create staged save migration %s\n",
                staged);
        /* The directory IS ours -- make_private_directory just created it on
         * this line of control flow. Do not re-derive ownership from the marker
         * file: write_migration_owner removes its own partial marker when it
         * fails, so a marker test here reads as "not ours" and leaks the empty
         * stage directory forever. Verify only that nothing swapped a symlink
         * in underneath us. */
        if (path_is_owned_stage_directory(staged)) {
            cleanup_staged_save(staged);
        }
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
    remove_installed_migration_owner(destination);
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

    set_last_error(NULL, NULL);
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
    set_last_error(NULL, NULL);
    if (!s_packaged) {
        return 1;
    }
    if (!s_pref_ready) {
        return 0;
    }
    return migrate_video_config() && migrate_save_directory();
#endif
}

const char *mdkr_user_paths_last_error(void) {
    return s_last_error;
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

int mdkr_user_mods_directory(char *output, size_t output_size) {
#ifdef __EMSCRIPTEN__
    /* The browser build ships no writable content root; the path resolves so
     * the caller has one code path, and the scan finds nothing. */
    return path_copy(output, output_size, "/mods");
#else
    if (s_packaged) {
        return s_pref_ready && path_join(output, output_size, s_pref_dir, "mods");
    }
    return path_copy(output, output_size, "mods");
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
