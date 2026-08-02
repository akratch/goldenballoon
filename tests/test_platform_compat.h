#ifndef MDKR_TEST_PLATFORM_COMPAT_H
#define MDKR_TEST_PLATFORM_COMPAT_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

static inline int mdkr_test_env_set(const char *name, const char *value,
                                    int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(name) != NULL) {
        return 0;
    }
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

static inline int mdkr_test_env_unset(const char *name) {
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

static inline int mdkr_test_make_directory(const char *path) {
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static inline int mdkr_test_make_temp_directory(char *output, size_t capacity,
                                                const char *prefix) {
    if (output == NULL || capacity == 0 || prefix == NULL || prefix[0] == '\0') {
        return 0;
    }
#ifdef _WIN32
    char base[MAX_PATH + 1];
    char candidate[MAX_PATH + 1];
    DWORD length = GetTempPathA((DWORD) sizeof(base), base);
    if (length == 0 || length >= sizeof(base) ||
        GetTempFileNameA(base, "mdk", 0, candidate) == 0) {
        return 0;
    }
    if (!DeleteFileA(candidate) || !CreateDirectoryA(candidate, NULL)) {
        (void) DeleteFileA(candidate);
        return 0;
    }
    int written = snprintf(output, capacity, "%s", candidate);
    if (written < 0 || (size_t) written >= capacity) {
        (void) RemoveDirectoryA(candidate);
        return 0;
    }
    return 1;
#else
    int written = snprintf(output, capacity, "/tmp/%s-XXXXXX", prefix);
    return written >= 0 && (size_t) written < capacity &&
           mkdtemp(output) != NULL;
#endif
}

#endif
