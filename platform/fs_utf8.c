#include "fs_utf8.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Platform-independent so the rule stays testable everywhere the suite runs;
 * only the Windows exec arm below consumes it. See fs_utf8.h. */
int mdkr_windows_quote_argument_utf8(const char *argument, char *output,
                                     size_t capacity) {
    size_t written = 0u;
    size_t index;
    size_t backslashes;

    if (argument == NULL || output == NULL || capacity == 0u) {
        errno = EINVAL;
        return -1;
    }
#define MDKR_QUOTE_EMIT(character)                 \
    do {                                           \
        if (written + 1u >= capacity) {            \
            output[0] = '\0';                      \
            errno = ENAMETOOLONG;                  \
            return -1;                             \
        }                                          \
        output[written++] = (character);           \
    } while (0)

    MDKR_QUOTE_EMIT('"');
    for (index = 0u; argument[index] != '\0'; ++index) {
        if (argument[index] == '\\') continue;
        /* Count the backslash run that ends here, then decide what it means. */
        for (backslashes = 0u;
             backslashes < index && argument[index - backslashes - 1u] == '\\';
             ++backslashes) {
        }
        if (argument[index] == '"') {
            /* A backslash run immediately before a quote is halved by the
             * parser, so emit it twice, then escape the quote itself. */
            size_t repeat;
            for (repeat = 0u; repeat < backslashes * 2u; ++repeat) {
                MDKR_QUOTE_EMIT('\\');
            }
            MDKR_QUOTE_EMIT('\\');
            MDKR_QUOTE_EMIT('"');
        } else {
            size_t repeat;
            for (repeat = 0u; repeat < backslashes; ++repeat) {
                MDKR_QUOTE_EMIT('\\');
            }
            MDKR_QUOTE_EMIT(argument[index]);
        }
    }
    /* Trailing backslashes precede the closing quote, so they double too. */
    for (backslashes = 0u;
         backslashes < index && argument[index - backslashes - 1u] == '\\';
         ++backslashes) {
    }
    {
        size_t repeat;
        for (repeat = 0u; repeat < backslashes * 2u; ++repeat) {
            MDKR_QUOTE_EMIT('\\');
        }
    }
    MDKR_QUOTE_EMIT('"');
#undef MDKR_QUOTE_EMIT
    output[written] = '\0';
    return (int)written;
}

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <wchar.h>
#include <windows.h>

static void fail_windows(DWORD error) {
    (void)error;
    /* MinGW and MSVC do not expose one common public Win32->errno mapper. The
     * underlying wide CRT calls set errno themselves; direct Win32 calls use a
     * stable generic failure while their callers retain GetLastError details. */
    errno = EIO;
}

static wchar_t *utf8_to_extended_path(const char *path) {
    wchar_t *wide = NULL;
    wchar_t *absolute = NULL;
    wchar_t *extended = NULL;
    DWORD absolute_size;
    int wide_size;
    size_t length;
    size_t prefix;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    path, -1, NULL, 0);
    if (wide_size <= 0) {
        errno = EINVAL;
        return NULL;
    }
    wide = (wchar_t *)calloc((size_t)wide_size, sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide, wide_size) <= 0) {
        free(wide);
        errno = EINVAL;
        return NULL;
    }
    for (int index = 0; wide[index] != L'\0'; ++index) {
        if (wide[index] == L'/') wide[index] = L'\\';
    }
    if (wcsncmp(wide, L"\\\\?\\", 4) == 0) return wide;

    absolute_size = GetFullPathNameW(wide, 0, NULL, NULL);
    if (absolute_size == 0) {
        free(wide);
        fail_windows(GetLastError());
        return NULL;
    }
    absolute = (wchar_t *)calloc((size_t)absolute_size, sizeof(*absolute));
    if (absolute == NULL) {
        free(wide);
        return NULL;
    }
    if (GetFullPathNameW(wide, absolute_size, absolute, NULL) == 0) {
        free(wide);
        free(absolute);
        fail_windows(GetLastError());
        return NULL;
    }
    free(wide);

    length = wcslen(absolute);
    prefix = wcsncmp(absolute, L"\\\\", 2) == 0 ? 8u : 4u;
    extended = (wchar_t *)calloc(prefix + length + 1u, sizeof(*extended));
    if (extended == NULL) {
        free(absolute);
        return NULL;
    }
    if (prefix == 8u) {
        wcscpy(extended, L"\\\\?\\UNC\\");
        wcscat(extended, absolute + 2);
    } else {
        wcscpy(extended, L"\\\\?\\");
        wcscat(extended, absolute);
    }
    free(absolute);
    return extended;
}

static int wide_to_utf8_alloc(const wchar_t *wide, char **output) {
    int needed;
    char *converted;
    if (wide == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                 NULL, 0, NULL, NULL);
    if (needed <= 0) {
        errno = EIO;
        return -1;
    }
    converted = (char *)malloc((size_t)needed);
    if (converted == NULL) return -1;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                            converted, needed, NULL, NULL) <= 0) {
        free(converted);
        errno = EIO;
        return -1;
    }
    *output = converted;
    return 0;
}

static int ascii_mode(const char *mode, wchar_t output[16]) {
    size_t length;
    if (mode == NULL || (length = strlen(mode)) == 0 || length >= 16u) {
        errno = EINVAL;
        return 0;
    }
    for (size_t index = 0; index <= length; ++index) {
        if ((unsigned char)mode[index] > 0x7fu) {
            errno = EINVAL;
            return 0;
        }
        output[index] = (wchar_t)(unsigned char)mode[index];
    }
    return 1;
}

FILE *mdkr_fopen_utf8(const char *path, const char *mode) {
    wchar_t wide_mode[16];
    wchar_t *wide_path;
    FILE *file;
    if (!ascii_mode(mode, wide_mode)) return NULL;
    wide_path = utf8_to_extended_path(path);
    if (wide_path == NULL) return NULL;
    file = _wfopen(wide_path, wide_mode);
    free(wide_path);
    return file;
}

int mdkr_remove_utf8(const char *path) {
    wchar_t *wide = utf8_to_extended_path(path);
    int result;
    if (wide == NULL) return -1;
    result = _wremove(wide);
    free(wide);
    return result;
}

int mdkr_unlink_utf8(const char *path) {
    wchar_t *wide = utf8_to_extended_path(path);
    int result;
    if (wide == NULL) return -1;
    result = _wunlink(wide);
    free(wide);
    return result;
}

int mdkr_mkdir_utf8(const char *path) {
    wchar_t *wide = utf8_to_extended_path(path);
    int result;
    if (wide == NULL) return -1;
    result = _wmkdir(wide);
    free(wide);
    return result;
}

int mdkr_rmdir_utf8(const char *path) {
    wchar_t *wide = utf8_to_extended_path(path);
    int result;
    if (wide == NULL) return -1;
    result = _wrmdir(wide);
    free(wide);
    return result;
}

int mdkr_move_utf8(const char *from, const char *to,
                   int replace, int write_through) {
    wchar_t *wide_from = utf8_to_extended_path(from);
    wchar_t *wide_to = utf8_to_extended_path(to);
    DWORD flags = (replace ? MOVEFILE_REPLACE_EXISTING : 0u) |
                  (write_through ? MOVEFILE_WRITE_THROUGH : 0u);
    int result = -1;
    if (wide_from != NULL && wide_to != NULL) {
        if (MoveFileExW(wide_from, wide_to, flags)) result = 0;
        else fail_windows(GetLastError());
    }
    free(wide_from);
    free(wide_to);
    return result;
}

int mdkr_path_query_utf8(const char *path, int *exists,
                         int *is_regular, int *is_directory) {
    wchar_t *wide = utf8_to_extended_path(path);
    DWORD attributes;
    if (exists) *exists = 0;
    if (is_regular) *is_regular = 0;
    if (is_directory) *is_directory = 0;
    if (wide == NULL) return -1;
    attributes = GetFileAttributesW(wide);
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (exists) *exists = 1;
    if (is_directory) *is_directory =
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_regular) *is_regular =
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return 0;
}

int mdkr_path_is_link_or_reparse_utf8(const char *path) {
    wchar_t *wide = utf8_to_extended_path(path);
    DWORD attributes;
    if (wide == NULL) return -1;
    attributes = GetFileAttributesW(wide);
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) return -1;
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? 1 : 0;
}

/* Convert one UTF-8 argument into the quoted wide form the Windows parser
 * recovers verbatim. Returns NULL and leaves errno set on failure. */
static wchar_t *quoted_wide_argument(const char *argument) {
    char *quoted;
    wchar_t *wide;
    int quoted_length;
    int wide_size;
    size_t capacity;

    if (argument == NULL) {
        errno = EINVAL;
        return NULL;
    }
    /* Worst case is every byte being a backslash that ends up doubled, plus
     * the surrounding quotes and the terminator. */
    capacity = strlen(argument) * 2u + 3u;
    quoted = (char *)calloc(capacity, sizeof(*quoted));
    if (quoted == NULL) return NULL;
    quoted_length =
        mdkr_windows_quote_argument_utf8(argument, quoted, capacity);
    if (quoted_length < 0) {
        const int saved = errno;
        free(quoted);
        errno = saved ? saved : EINVAL;
        return NULL;
    }
    wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    quoted, -1, NULL, 0);
    if (wide_size <= 0) {
        free(quoted);
        errno = EINVAL;
        return NULL;
    }
    wide = (wchar_t *)calloc((size_t)wide_size, sizeof(*wide));
    if (wide == NULL) {
        free(quoted);
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, quoted, -1,
                            wide, wide_size) <= 0) {
        free(quoted);
        free(wide);
        errno = EINVAL;
        return NULL;
    }
    free(quoted);
    return wide;
}

int mdkr_exec_replace_utf8(const char *path, const char *const *arguments) {
    /* The image itself is addressed through the extended-length form so a
     * package extracted into a deep or non-ASCII directory still starts. The
     * ARGUMENT vector is separate: the CRT joins it into the replacement's
     * command line with single spaces and no quoting whatsoever, so every
     * element -- argv[0] included -- has to arrive already quoted or an
     * install path containing a space reaches the replacement as several
     * arguments, which arg_triage then reads as "the caller already told the
     * engine what to do". That is a windowless start, not a launcher. */
    wchar_t *image = utf8_to_extended_path(path);
    wchar_t **vector = NULL;
    size_t count = 0u;
    size_t index;
    int saved;

    if (image == NULL) return errno ? errno : EINVAL;
    if (arguments != NULL) {
        while (arguments[count] != NULL) ++count;
    }
    vector = (wchar_t **)calloc(count + 2u, sizeof(*vector));
    if (vector == NULL) {
        saved = errno ? errno : ENOMEM;
        free(image);
        return saved;
    }
    /* argv[0] is the launch spelling the replacement reports, not the image
     * lookup: keep it the plain path so nothing downstream inherits a \\?\
     * prefix it cannot interpret. */
    vector[0] = quoted_wide_argument(path);
    for (index = 0u; index < count && vector[index] != NULL; ++index) {
        vector[index + 1u] = quoted_wide_argument(arguments[index]);
    }
    if (vector[count] == NULL) {
        size_t discard;
        saved = errno ? errno : EINVAL;
        for (discard = 0u; discard <= count; ++discard) free(vector[discard]);
        free(vector);
        free(image);
        return saved;
    }

    _wexecv(image, (const wchar_t *const *)vector);
    saved = errno ? errno : EIO;
    for (index = 0u; index <= count; ++index) free(vector[index]);
    free(vector);
    free(image);
    return saved;
}

int mdkr_file_sync(FILE *file) {
    if (file == NULL || fflush(file) != 0 || _commit(_fileno(file)) != 0) {
        return -1;
    }
    return 0;
}

int mdkr_parent_directory_sync_utf8(const char *path) {
    /* MOVEFILE_WRITE_THROUGH is the Windows directory-entry durability boundary.
     * Windows has no portable directory handle equivalent for this operation. */
    (void)path;
    return 0;
}

int mdkr_file_lock_acquire_utf8(const char *path, MdkrFileLock *lock) {
    wchar_t *wide;
    HANDLE handle;
    OVERLAPPED overlapped;
    if (lock == NULL) {
        errno = EINVAL;
        return -1;
    }
    lock->handle = (intptr_t)-1;
    wide = utf8_to_extended_path(path);
    if (wide == NULL) return -1;
    handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        fail_windows(GetLastError());
        return -1;
    }
    memset(&overlapped, 0, sizeof(overlapped));
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                    &overlapped)) {
        fail_windows(GetLastError());
        CloseHandle(handle);
        return -1;
    }
    lock->handle = (intptr_t)handle;
    return 0;
}

void mdkr_file_lock_release(MdkrFileLock *lock) {
    HANDLE handle;
    OVERLAPPED overlapped;
    if (lock == NULL || lock->handle == (intptr_t)-1) return;
    handle = (HANDLE)lock->handle;
    memset(&overlapped, 0, sizeof(overlapped));
    (void)UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle);
    lock->handle = (intptr_t)-1;
}

int mdkr_running_executable_path_utf8(char **output) {
    DWORD size = MAX_PATH;
    wchar_t *wide = NULL;
    if (output == NULL) {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    for (;;) {
        DWORD written;
        wchar_t *grown = (wchar_t *)realloc(wide, (size_t)size * sizeof(*wide));
        if (grown == NULL) {
            free(wide);
            return -1;
        }
        wide = grown;
        written = GetModuleFileNameW(NULL, wide, size);
        if (written == 0) {
            free(wide);
            fail_windows(GetLastError());
            return -1;
        }
        /* A successful modern Windows call returns the length excluding its
         * terminator, so `written == size - 1` is a valid full buffer, not a
         * truncation. Truncation returns size (with no terminator). */
        if (written < size) break;
        if (size >= 32768u) {
            free(wide);
            errno = ENAMETOOLONG;
            return -1;
        }
        size = size > 16384u ? 32768u : size * 2u;
    }
    {
        const int result = wide_to_utf8_alloc(wide, output);
        free(wide);
        return result;
    }
}

#else

#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <limits.h>
#include <stdint.h>
#include <mach-o/dyld.h>
#endif

FILE *mdkr_fopen_utf8(const char *path, const char *mode) {
    return fopen(path, mode);
}
int mdkr_remove_utf8(const char *path) { return remove(path); }
int mdkr_unlink_utf8(const char *path) { return unlink(path); }
int mdkr_mkdir_utf8(const char *path) { return mkdir(path, 0700); }
int mdkr_rmdir_utf8(const char *path) { return rmdir(path); }
int mdkr_move_utf8(const char *from, const char *to,
                   int replace, int write_through) {
    (void)write_through;
    if (!replace) {
        int saved;
        /* The non-replacing native call sites move regular files. link()+
         * unlink() gives them an atomic no-clobber install on POSIX, matching
         * MOVEFILE_REPLACE_EXISTING being absent on Windows. Directory
         * migration uses its platform-specific exclusive rename separately. */
        if (link(from, to) != 0) return -1;
        if (unlink(from) == 0) return 0;
        saved = errno;
        (void)unlink(to);
        errno = saved;
        return -1;
    }
    return rename(from, to);
}
int mdkr_path_query_utf8(const char *path, int *exists,
                         int *is_regular, int *is_directory) {
    struct stat status;
    int found = path != NULL && stat(path, &status) == 0;
    if (exists) *exists = found;
    if (is_regular) *is_regular = found && S_ISREG(status.st_mode);
    if (is_directory) *is_directory = found && S_ISDIR(status.st_mode);
    return found ? 0 : -1;
}
int mdkr_path_is_link_or_reparse_utf8(const char *path) {
    struct stat status;
    if (path == NULL || lstat(path, &status) != 0) return -1;
    return S_ISLNK(status.st_mode) ? 1 : 0;
}
int mdkr_exec_replace_utf8(const char *path, const char *const *arguments) {
    /* POSIX carries the argument vector across exec verbatim, so there is no
     * command line to quote; the vector is still built explicitly so both arms
     * hand the replacement exactly the same argv. */
    char **vector;
    size_t count = 0u;
    size_t index;
    int saved;

    if (path == NULL || path[0] == '\0') return EINVAL;
    if (arguments != NULL) {
        while (arguments[count] != NULL) ++count;
    }
    vector = (char **)calloc(count + 2u, sizeof(*vector));
    if (vector == NULL) return errno ? errno : ENOMEM;
    vector[0] = (char *)path;
    for (index = 0u; index < count; ++index) {
        vector[index + 1u] = (char *)arguments[index];
    }
    execvp(path, vector);
    saved = errno ? errno : EIO;
    free(vector);
    return saved;
}
int mdkr_file_sync(FILE *file) {
    if (file == NULL || fflush(file) != 0) return -1;
#ifdef __EMSCRIPTEN__
    return 0;
#else
    return fsync(fileno(file)) == 0 ? 0 : -1;
#endif
}
int mdkr_parent_directory_sync_utf8(const char *path) {
#ifdef __EMSCRIPTEN__
    /* IDBFS durability is committed by the shell's asynchronous syncfs owner. */
    (void)path;
    return 0;
#else
    const char *slash;
    size_t length;
    char *parent;
    int descriptor;
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    slash = strrchr(path, '/');
    length = slash == NULL ? 1u :
             slash == path ? 1u : (size_t)(slash - path);
    parent = (char *)malloc(length + 1u);
    if (parent == NULL) return -1;
    if (slash == NULL) memcpy(parent, ".", length);
    else memcpy(parent, path, length);
    parent[length] = '\0';
    descriptor = open(parent, O_RDONLY | O_DIRECTORY);
    free(parent);
    if (descriptor < 0) return -1;
    if (fsync(descriptor) != 0) {
        const int saved = errno;
        close(descriptor);
        errno = saved;
        return -1;
    }
    return close(descriptor);
#endif
}
int mdkr_file_lock_acquire_utf8(const char *path, MdkrFileLock *lock) {
    int descriptor;
    if (path == NULL || lock == NULL) {
        errno = EINVAL;
        return -1;
    }
    lock->handle = -1;
    descriptor = open(path, O_CREAT | O_RDWR, 0600);
    if (descriptor < 0) return -1;
    if (flock(descriptor, LOCK_EX) != 0) {
        const int saved = errno;
        close(descriptor);
        errno = saved;
        return -1;
    }
    lock->handle = (intptr_t)descriptor;
    return 0;
}
void mdkr_file_lock_release(MdkrFileLock *lock) {
    const int descriptor = lock != NULL ? (int)lock->handle : -1;
    if (descriptor < 0) return;
    (void)flock(descriptor, LOCK_UN);
    (void)close(descriptor);
    lock->handle = -1;
}
int mdkr_running_executable_path_utf8(char **output) {
    if (output == NULL) {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
#if defined(__APPLE__)
    {
        char stack_path[PATH_MAX];
        uint32_t size = (uint32_t) sizeof(stack_path);
        char *path;
        if (_NSGetExecutablePath(stack_path, &size) == 0) {
            path = (char *) malloc(strlen(stack_path) + 1u);
            if (path == NULL) return -1;
            memcpy(path, stack_path, strlen(stack_path) + 1u);
            *output = path;
            return 0;
        }
        /* A path longer than PATH_MAX reports the size it needs. */
        path = (char *) malloc((size_t) size);
        if (path == NULL) return -1;
        if (_NSGetExecutablePath(path, &size) != 0) {
            free(path);
            errno = ENOENT;
            return -1;
        }
        *output = path;
        return 0;
    }
#elif defined(__linux__)
    {
        /* readlink never terminates and never reports the required length, so
         * a result that exactly fills the buffer is indistinguishable from a
         * truncated one: grow and retry rather than return a cut path. */
        size_t capacity;
        for (capacity = 1024u; capacity <= 262144u; capacity *= 2u) {
            char *path = (char *) malloc(capacity);
            ssize_t written;
            if (path == NULL) return -1;
            written = readlink("/proc/self/exe", path, capacity - 1u);
            if (written < 0) {
                const int saved = errno;
                free(path);
                errno = saved;
                return -1;
            }
            if ((size_t) written < capacity - 1u) {
                path[written] = '\0';
                *output = path;
                return 0;
            }
            free(path);
        }
        errno = ENAMETOOLONG;
        return -1;
    }
#else
    errno = ENOSYS;
    return -1;
#endif
}

#endif
