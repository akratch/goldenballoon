#include "taj_mod_state_file.h"

#include "fs_utf8.h"
#include "taj_mod.h"
#include "user_paths.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <process.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
EM_JS(void, taj_mod_state_schedule_web_persist, (unsigned int generation), {
    const persist = (typeof Module.__mdkrPersist === "function")
        ? Module.__mdkrPersist({reason: "taj-mod", urgent: true})
        : new Promise((resolve, reject) => {
            FS.syncfs(false, error => error ? reject(error) : resolve());
        });
    Promise.resolve(persist).then(() => {
        if (typeof Module._taj_mod_report_persistence_success === "function") {
            Module._taj_mod_report_persistence_success(generation);
        }
    }).catch(error => {
        if (typeof Module._taj_mod_report_persistence_failure === "function") {
            Module._taj_mod_report_persistence_failure(generation);
        }
        if (typeof Module.__mdkrPersistFailed === "function") {
            Module.__mdkrPersistFailed(String(
                error && error.message ? error.message : error));
        }
    });
});
#endif

enum {
    TAJ_MOD_STATE_INITIAL_PATH_CAPACITY = 256,
    /* Windows extended paths can require almost 128 KiB of UTF-8. */
    TAJ_MOD_STATE_MAX_PATH_CAPACITY = 256 * 1024
};

typedef struct TajModStatePaths {
    char *directory;
    char *path;
    char *lock;
} TajModStatePaths;

static unsigned long s_taj_mod_state_temp_serial;

static void taj_mod_state_paths_dispose(TajModStatePaths *paths) {
    if (paths == NULL) return;
    free(paths->lock);
    free(paths->path);
    free(paths->directory);
    memset(paths, 0, sizeof(*paths));
}

static char *taj_mod_state_path_append(const char *path, const char *suffix) {
    const size_t path_length = path != NULL ? strlen(path) : 0;
    const size_t suffix_length = suffix != NULL ? strlen(suffix) : 0;
    char *result;

    if (path == NULL || suffix == NULL ||
        path_length > SIZE_MAX - suffix_length - 1u) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    result = (char *)malloc(path_length + suffix_length + 1u);
    if (result == NULL) return NULL;
    memcpy(result, path, path_length);
    memcpy(result + path_length, suffix, suffix_length + 1u);
    return result;
}

static int taj_mod_state_paths_init(TajModStatePaths *paths) {
    size_t capacity = TAJ_MOD_STATE_INITIAL_PATH_CAPACITY;

    if (paths == NULL) {
        errno = EINVAL;
        return 0;
    }
    memset(paths, 0, sizeof(*paths));
    while (capacity <= TAJ_MOD_STATE_MAX_PATH_CAPACITY) {
        char *directory = (char *)malloc(capacity);
        if (directory == NULL) return 0;
        if (mdkr_user_save_directory(directory, capacity)) {
            paths->directory = directory;
            break;
        }
        free(directory);
        if (capacity > TAJ_MOD_STATE_MAX_PATH_CAPACITY / 2u) break;
        capacity *= 2u;
    }
    if (paths->directory == NULL) {
        errno = ENAMETOOLONG;
        return 0;
    }
    paths->path = taj_mod_state_path_append(paths->directory,
                                            "/taj_mod_state.ini");
    if (paths->path != NULL) {
        paths->lock = taj_mod_state_path_append(paths->path, ".lock");
    }
    if (paths->path == NULL || paths->lock == NULL) {
        taj_mod_state_paths_dispose(paths);
        return 0;
    }
    return 1;
}

static long taj_mod_state_process_id(void) {
#if defined(__EMSCRIPTEN__)
    return 1;
#elif defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int taj_mod_state_open_unique_temp(const char *path, char **temporary,
                                          FILE **file) {
    unsigned int attempt;

    if (path == NULL || temporary == NULL || file == NULL) {
        errno = EINVAL;
        return 0;
    }
    *temporary = NULL;
    *file = NULL;
    for (attempt = 0; attempt < 64u; ++attempt) {
        char suffix[96];
        int written = snprintf(suffix, sizeof(suffix), ".tmp.%ld.%lu",
                               taj_mod_state_process_id(),
                               ++s_taj_mod_state_temp_serial);
        if (written < 0 || (size_t)written >= sizeof(suffix)) {
            errno = ENAMETOOLONG;
            return 0;
        }
        *temporary = taj_mod_state_path_append(path, suffix);
        if (*temporary == NULL) return 0;
        *file = mdkr_fopen_utf8(*temporary, "wbx");
        if (*file != NULL) return 1;
        free(*temporary);
        *temporary = NULL;
        if (errno != EEXIST) break;
    }
    return 0;
}

static int taj_mod_state_ensure_directory(const char *directory) {
    int exists = 0;
    int is_directory = 0;

    if (mdkr_path_query_utf8(directory, &exists, NULL, &is_directory) == 0) {
        return is_directory;
    }
    if (mdkr_mkdir_utf8(directory) != 0 && errno != EEXIST) {
        return 0;
    }
    return mdkr_path_query_utf8(directory, &exists, NULL, &is_directory) == 0 &&
           exists && is_directory;
}

static int taj_mod_state_file_read(void *context, char *text, size_t capacity,
                                   size_t *length) {
    TajModStatePaths paths;
    FILE *file;
    size_t bytes;
    int extra;
    int read_failed;
    int close_failed;
    int result;

    (void)context;
    if (text == NULL || capacity == 0 || length == NULL ||
        !taj_mod_state_paths_init(&paths)) {
        return -1;
    }
    file = mdkr_fopen_utf8(paths.path, "rb");
    if (file == NULL) {
        result = errno == ENOENT ? 0 : -1;
        taj_mod_state_paths_dispose(&paths);
        return result;
    }
    bytes = fread(text, 1, capacity, file);
    extra = fgetc(file);
    read_failed = ferror(file) != 0;
    close_failed = fclose(file) != 0;
    if (read_failed || close_failed || bytes == capacity || extra != EOF) {
        taj_mod_state_paths_dispose(&paths);
        return -1;
    }
    *length = bytes;
    taj_mod_state_paths_dispose(&paths);
    return 1;
}

static int taj_mod_state_file_write(void *context, const char *text, size_t length) {
    TajModStatePaths paths;
    MdkrFileLock lock;
    char *temporary = NULL;
    FILE *file = NULL;
    int failed = 0;
    int moved = 0;
    int result = -1;

    (void)context;
    lock.handle = (intptr_t)-1;
    if (text == NULL || !taj_mod_state_paths_init(&paths)) return -1;
    if (!taj_mod_state_ensure_directory(paths.directory)) goto done;
#ifndef __EMSCRIPTEN__
    if (mdkr_file_lock_acquire_utf8(paths.lock, &lock) != 0) {
        goto done;
    }
#endif
    if (!taj_mod_state_open_unique_temp(paths.path, &temporary, &file)) {
        goto done;
    }
    failed = fwrite(text, 1, length, file) != length ||
             mdkr_file_sync(file) != 0;
    if (fclose(file) != 0) {
        failed = 1;
    }
    file = NULL;
    if (failed || mdkr_move_utf8(temporary, paths.path, 1, 1) != 0) {
        goto done;
    }
    moved = 1;
    if (mdkr_parent_directory_sync_utf8(paths.path) != 0) goto done;
#ifdef __EMSCRIPTEN__
    taj_mod_state_schedule_web_persist(
        taj_mod_persistence_pending_generation());
#endif
    result = 1;

done:
    if (file != NULL) (void)fclose(file);
    if (!moved && temporary != NULL) (void)mdkr_remove_utf8(temporary);
    free(temporary);
    mdkr_file_lock_release(&lock);
    taj_mod_state_paths_dispose(&paths);
    return result;
}

const TajModStateStorage *taj_mod_state_file_storage(void) {
    static const TajModStateStorage storage = {
        NULL, taj_mod_state_file_read, taj_mod_state_file_write
    };
    return &storage;
}
