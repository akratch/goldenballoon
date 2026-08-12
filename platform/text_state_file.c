#include "text_state_file.h"

#include "fs_utf8.h"
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

enum {
    TEXT_STATE_INITIAL_PATH_CAPACITY = 256,
    TEXT_STATE_MAX_PATH_CAPACITY = 256 * 1024
};

typedef struct TextStatePaths {
    char *directory;
    char *path;
    char *lock;
} TextStatePaths;

static unsigned long s_text_state_temp_serial;

static void text_state_paths_dispose(TextStatePaths *paths) {
    if (paths == NULL) return;
    free(paths->lock);
    free(paths->path);
    free(paths->directory);
    memset(paths, 0, sizeof(*paths));
}

static char *text_state_path_append(const char *path, const char *suffix) {
    const size_t path_length = path != NULL ? strlen(path) : 0u;
    const size_t suffix_length = suffix != NULL ? strlen(suffix) : 0u;
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

static int text_state_filename_valid(const char *filename) {
    return filename != NULL && filename[0] != '\0' &&
           strchr(filename, '/') == NULL && strchr(filename, '\\') == NULL &&
           strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0;
}

static int text_state_paths_init(TextStatePaths *paths,
                                 const MdkrTextStateFileSpec *spec) {
    size_t capacity = TEXT_STATE_INITIAL_PATH_CAPACITY;

    if (paths == NULL || spec == NULL ||
        !text_state_filename_valid(spec->filename)) {
        errno = EINVAL;
        return 0;
    }
    memset(paths, 0, sizeof(*paths));
    while (capacity <= TEXT_STATE_MAX_PATH_CAPACITY) {
        char *directory = (char *)malloc(capacity);
        if (directory == NULL) return 0;
        if (mdkr_user_save_directory(directory, capacity)) {
            paths->directory = directory;
            break;
        }
        free(directory);
        if (capacity > TEXT_STATE_MAX_PATH_CAPACITY / 2u) break;
        capacity *= 2u;
    }
    if (paths->directory == NULL) {
        errno = ENAMETOOLONG;
        return 0;
    }
    paths->path = text_state_path_append(paths->directory, "/");
    if (paths->path != NULL) {
        char *complete = text_state_path_append(paths->path, spec->filename);
        free(paths->path);
        paths->path = complete;
    }
    if (paths->path != NULL) {
        paths->lock = text_state_path_append(paths->path, ".lock");
    }
    if (paths->path == NULL || paths->lock == NULL) {
        text_state_paths_dispose(paths);
        return 0;
    }
    return 1;
}

static long text_state_process_id(void) {
#if defined(__EMSCRIPTEN__)
    return 1;
#elif defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int text_state_open_unique_temp(const char *path, char **temporary,
                                       FILE **file) {
    unsigned attempt;

    if (path == NULL || temporary == NULL || file == NULL) {
        errno = EINVAL;
        return 0;
    }
    *temporary = NULL;
    *file = NULL;
    for (attempt = 0u; attempt < 64u; attempt++) {
        char suffix[96];
        int written = snprintf(suffix, sizeof(suffix), ".tmp.%ld.%lu",
                               text_state_process_id(),
                               ++s_text_state_temp_serial);
        if (written < 0 || (size_t)written >= sizeof(suffix)) {
            errno = ENAMETOOLONG;
            return 0;
        }
        *temporary = text_state_path_append(path, suffix);
        if (*temporary == NULL) return 0;
        *file = mdkr_fopen_utf8(*temporary, "wbx");
        if (*file != NULL) return 1;
        free(*temporary);
        *temporary = NULL;
        if (errno != EEXIST) break;
    }
    return 0;
}

static int text_state_ensure_directory(const char *directory) {
    int exists = 0;
    int is_directory = 0;

    if (mdkr_path_query_utf8(directory, &exists, NULL, &is_directory) == 0) {
        return exists && is_directory;
    }
    if (mdkr_mkdir_utf8(directory) != 0 && errno != EEXIST) {
        return 0;
    }
    return mdkr_path_query_utf8(directory, &exists, NULL, &is_directory) == 0 &&
           exists && is_directory;
}

int mdkr_text_state_file_read(void *context, char *text, size_t capacity,
                              size_t *length) {
    const MdkrTextStateFileSpec *spec =
        (const MdkrTextStateFileSpec *)context;
    TextStatePaths paths;
    FILE *file;
    size_t bytes;
    int extra;
    int failed;
    int result;

    if (text == NULL || capacity == 0u || length == NULL ||
        !text_state_paths_init(&paths, spec)) {
        return -1;
    }
    file = mdkr_fopen_utf8(paths.path, "rb");
    if (file == NULL) {
        result = errno == ENOENT ? 0 : -1;
        text_state_paths_dispose(&paths);
        return result;
    }
    bytes = fread(text, 1u, capacity, file);
    extra = fgetc(file);
    failed = ferror(file) != 0 || fclose(file) != 0 ||
             bytes == capacity || extra != EOF;
    if (failed) {
        text_state_paths_dispose(&paths);
        return -1;
    }
    *length = bytes;
    text_state_paths_dispose(&paths);
    return 1;
}

int mdkr_text_state_file_write(void *context, const char *text, size_t length) {
    const MdkrTextStateFileSpec *spec =
        (const MdkrTextStateFileSpec *)context;
    TextStatePaths paths;
    MdkrFileLock lock;
    char *temporary = NULL;
    FILE *file = NULL;
    int failed;
    int moved = 0;
    int result = -1;

    lock.handle = (intptr_t)-1;
    if (text == NULL || !text_state_paths_init(&paths, spec)) return -1;
    if (!text_state_ensure_directory(paths.directory)) goto done;
#ifndef __EMSCRIPTEN__
    if (mdkr_file_lock_acquire_utf8(paths.lock, &lock) != 0) goto done;
#endif
    if (!text_state_open_unique_temp(paths.path, &temporary, &file)) goto done;
    failed = fwrite(text, 1u, length, file) != length ||
             mdkr_file_sync(file) != 0;
    if (fclose(file) != 0) failed = 1;
    file = NULL;
    if (failed || mdkr_move_utf8(temporary, paths.path, 1, 1) != 0) goto done;
    moved = 1;
    if (mdkr_parent_directory_sync_utf8(paths.path) != 0) goto done;
    if (spec->did_replace != NULL) {
        spec->did_replace(spec->did_replace_context);
    }
    result = 1;

done:
    if (file != NULL) (void)fclose(file);
    if (!moved && temporary != NULL) (void)mdkr_remove_utf8(temporary);
    free(temporary);
    mdkr_file_lock_release(&lock);
    text_state_paths_dispose(&paths);
    return result;
}
