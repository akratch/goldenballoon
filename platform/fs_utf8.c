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

/* The length of the well-formed UTF-8 sequence that starts at `cursor` -- no
 * stray continuation byte, no truncated or overlong sequence, no UTF-16
 * surrogate, nothing past U+10FFFF -- or 0 when none starts there. */
static size_t utf8_sequence_length(const unsigned char *cursor) {
    const unsigned lead = cursor[0];
    unsigned long point;
    size_t extra;
    size_t index;
    if (lead < 0x80u) return 1u;
    if (lead >= 0xC2u && lead <= 0xDFu) {
        extra = 1u;
        point = lead & 0x1Fu;
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
        extra = 2u;
        point = lead & 0x0Fu;
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
        extra = 3u;
        point = lead & 0x07u;
    } else {
        return 0u;
    }
    for (index = 1u; index <= extra; ++index) {
        /* A NUL is not a continuation byte, so this never reads past it. */
        if ((cursor[index] & 0xC0u) != 0x80u) return 0u;
        point = point * 64u + (cursor[index] & 0x3Fu);
    }
    if ((extra == 2u && point < 0x800u) || (extra == 3u && point < 0x10000u) ||
        point > 0x10FFFFu || (point >= 0xD800u && point <= 0xDFFFu)) {
        return 0u;
    }
    return extra + 1u;
}

/* Platform-independent: both quarantine call sites (the controller pak in
 * stubs_dkr.c, the ghost bank) share it, so a fix to one cannot miss the
 * other. See fs_utf8.h. */
int mdkr_set_aside_utf8(const char *path, char *bad_path,
                        size_t bad_path_size) {
    int slot;
    if (bad_path != NULL && bad_path_size > 0u) bad_path[0] = '\0';
    if (path == NULL || path[0] == '\0' || bad_path == NULL ||
        bad_path_size == 0u) {
        errno = EINVAL;
        return -1;
    }
    for (slot = 1; slot <= MDKR_SET_ASIDE_SLOTS; ++slot) {
        const int written =
            snprintf(bad_path, bad_path_size, "%s.bad.%d", path, slot);
        int kind;
        if (written < 0 || (size_t)written >= bad_path_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        kind = mdkr_path_kind_utf8(bad_path);
        if (kind < 0) return -1;
        if (kind != MDKR_PATH_MISSING) continue;
        if (mdkr_move_utf8(path, bad_path, 0, 1) == 0) return 1;
        /* Taken since the check: the no-replace move refused it. */
        if (errno != EEXIST) return -1;
    }
    return 0;
}

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#include <wchar.h>
#include <windows.h>

static void fail_windows(DWORD error) {
    /* MinGW exposes no public Win32->errno mapper. Map the failures a caller
     * can act on or explain; everything else stays a generic I/O error. */
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        errno = ENOENT;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
        errno = EACCES;
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_BUSY:
        errno = EBUSY;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_NOT_SAME_DEVICE:
        errno = EXDEV;
        break;
    /* The file system does not implement the operation at all (some network
     * and virtual drives). FAT and exFAT do implement rename, including the
     * refusal of an existing name, so this is not a FAT/exFAT answer. */
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        errno = ENOTSUP;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
        errno = EINVAL;
        break;
    case ERROR_FILENAME_EXCED_RANGE:
        errno = ENAMETOOLONG;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        errno = ENOMEM;
        break;
    default:
        errno = EIO;
        break;
    }
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

/* `flags` is WC_ERR_INVALID_CHARS to refuse a name that is not valid UTF-16,
 * or 0 to replace what cannot convert with U+FFFD. */
static int wide_to_utf8_convert(const wchar_t *wide, char **output, DWORD flags) {
    int needed;
    char *converted;
    if (wide == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    needed = WideCharToMultiByte(CP_UTF8, flags, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        errno = EIO;
        return -1;
    }
    converted = (char *)malloc((size_t)needed);
    if (converted == NULL) return -1;
    if (WideCharToMultiByte(CP_UTF8, flags, wide, -1,
                            converted, needed, NULL, NULL) <= 0) {
        free(converted);
        errno = EIO;
        return -1;
    }
    *output = converted;
    return 0;
}

static int wide_to_utf8_alloc(const wchar_t *wide, char **output) {
    return wide_to_utf8_convert(wide, output, WC_ERR_INVALID_CHARS);
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

/*
 * C11 exclusive create, independent of which CRT the toolchain links.
 *
 * fopen's 'x' flag is honored by the UCRT but rejected wholesale (EINVAL,
 * NULL) by the legacy msvcrt.dll. Every atomic settings/preferences/state
 * writer in this tree opens its temp file with "wbx", so a build lane that
 * links msvcrt turns every save into a visible failure — that is exactly how
 * the withdrawn v1.2.1 Windows zip broke ROM-path and settings persistence
 * (issue #32). Exclusive create therefore goes through the OS-level _wsopen
 * primitive, whose _O_CREAT|_O_EXCL semantics are identical on both CRTs.
 */
static FILE *wide_fopen_exclusive(const wchar_t *wide_path, const char *mode) {
    wchar_t stripped[16];
    size_t out = 0u;
    int flags = _O_CREAT | _O_EXCL;
    int has_write = 0;
    int has_plus = 0;
    int has_binary = 0;
    int fd;
    FILE *file;
    size_t index;

    for (index = 0u; mode[index] != '\0'; ++index) {
        if (mode[index] == 'x') continue;
        if (mode[index] == 'w') has_write = 1;
        else if (mode[index] == 'b') has_binary = 1;
        else if (mode[index] == '+') has_plus = 1;
        stripped[out++] = (wchar_t)(unsigned char)mode[index];
    }
    stripped[out] = L'\0';
    if (!has_write) {
        /* C11 permits 'x' only with 'w'; match fopen's contract. */
        errno = EINVAL;
        return NULL;
    }
    flags |= has_plus ? _O_RDWR : _O_WRONLY;
    flags |= has_binary ? _O_BINARY : _O_TEXT;
    fd = _wsopen(wide_path, flags, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (fd < 0) return NULL; /* _wsopen sets errno; EEXIST when present. */
    file = _wfdopen(fd, stripped);
    if (file == NULL) {
        const int saved_errno = errno;
        (void)_close(fd);
        errno = saved_errno;
    }
    return file;
}

FILE *mdkr_fopen_utf8(const char *path, const char *mode) {
    wchar_t wide_mode[16];
    wchar_t *wide_path;
    FILE *file;
    if (!ascii_mode(mode, wide_mode)) return NULL;
    wide_path = utf8_to_extended_path(path);
    if (wide_path == NULL) return NULL;
    file = strchr(mode, 'x') != NULL
        ? wide_fopen_exclusive(wide_path, mode)
        : _wfopen(wide_path, wide_mode);
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

/* The failures that mean nothing is there (fs_utf8.h): the name or a folder
 * above it is absent, a folder above it is a file (NTFS answers that with
 * ERROR_PATH_NOT_FOUND or ERROR_DIRECTORY), or its drive does not exist --
 * POSIX's ENOENT and ENOTDIR. Everything else is a real error. */
static int windows_error_means_missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_DIRECTORY || error == ERROR_INVALID_DRIVE;
}

int mdkr_path_query_utf8(const char *path, int *exists,
                         int *is_regular, int *is_directory) {
    wchar_t *wide;
    DWORD attributes;
    if (exists) *exists = 0;
    if (is_regular) *is_regular = 0;
    if (is_directory) *is_directory = 0;
    wide = utf8_to_extended_path(path);
    if (wide == NULL) return -1;
    attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        free(wide);
        if (windows_error_means_missing(error)) return 0;
        fail_windows(error);
        return -1;
    }
    free(wide);
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

/* Only name surrogates (symbolic links, junctions, mount points) redirect a
 * path somewhere else. Cloud-file placeholders and deduplicated files are
 * reparse points too, but they hold ordinary data and are read in place. */
static int windows_entry_kind(DWORD attributes, DWORD tag) {
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        IsReparseTagNameSurrogate(tag)) {
        return MDKR_PATH_LINK;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return MDKR_PATH_DIRECTORY;
    if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0) return MDKR_PATH_OTHER;
    return MDKR_PATH_FILE;
}

int mdkr_path_kind_utf8(const char *path) {
    wchar_t *wide = utf8_to_extended_path(path);
    DWORD attributes;
    DWORD tag = 0;
    if (wide == NULL) return -1;
    attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        free(wide);
        /* What the query calls missing, and (fs_utf8.h) an unusable name. */
        if (windows_error_means_missing(error) || error == ERROR_INVALID_NAME ||
            error == ERROR_BAD_PATHNAME) {
            return MDKR_PATH_MISSING;
        }
        fail_windows(error);
        return -1;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        /* The attribute says "reparse point"; only the tag says which kind. A
         * point whose tag cannot be read is treated as a link, never followed. */
        WIN32_FIND_DATAW data;
        const HANDLE find = FindFirstFileW(wide, &data);
        if (find == INVALID_HANDLE_VALUE) {
            free(wide);
            return MDKR_PATH_LINK;
        }
        tag = data.dwReserved0;
        FindClose(find);
    }
    free(wide);
    return windows_entry_kind(attributes, tag);
}

#ifdef MDKR_FS_UTF8_TESTING
/* Test-only, defined for mdkr_fs_utf8_test alone: 1 makes the listing's
 * attribute query on its handle, 2 its directory query, answer as a file
 * system that does not implement that query (ERROR_INVALID_FUNCTION). */
int mdkr_fs_utf8_test_refused_query;
#define LISTING_QUERY_REFUSED(which) \
    (mdkr_fs_utf8_test_refused_query == (which) && \
     (SetLastError(ERROR_INVALID_FUNCTION), 1))
#else
#define LISTING_QUERY_REFUSED(which) 0
#endif

/* What a file system, redirector or filter driver answers for a query it does
 * not implement (FAT/exFAT and some network and virtual drives, for some
 * information classes). */
static int windows_query_unsupported(DWORD error) {
    return error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_LEVEL ||
           error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED;
}

enum { LISTING_HANDLE = 0, LISTING_FAILED = 1, LISTING_BY_FIND_FIRST = 2 };

/* The directory is opened by this code, with every sharing mode granted, and
 * listed through that handle. FindFirstFileW opens it with a sharing mode of
 * its own choosing, and where that mode leaves out FILE_SHARE_DELETE (Wine's
 * does) a directory held by mdkr_directory_hold_utf8 -- whose handle has DELETE
 * access -- cannot be listed at all, so the installer could not empty the
 * stage it holds. Only the entries are never followed: the directory itself
 * may be reached through a link, as opendir() allows on POSIX, because a
 * caller chooses a pack root that way. A directory that is itself a reparse
 * point (a link, or a cloud placeholder) is therefore opened again without
 * FILE_FLAG_OPEN_REPARSE_POINT, so the system resolves it as FindFirstFileW
 * did. The attributes come from GetFileInformationByHandle, which every file
 * system answers, and FILE_READ_ATTRIBUTES is requested because that query
 * needs it (it takes no part in sharing). A file system that refuses the open
 * or the query as unsupported is listed through FindFirstFileW, as before.
 * Returns LISTING_HANDLE with `*output` set, LISTING_FAILED with errno set, or
 * LISTING_BY_FIND_FIRST. */
static int open_directory_for_listing(const wchar_t *wide, HANDLE *output) {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    *output = INVALID_HANDLE_VALUE;
    for (;;) {
        BY_HANDLE_FILE_INFORMATION info;
        DWORD error;
        HANDLE directory = CreateFileW(
            wide, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, flags, NULL);
        if (directory == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (windows_query_unsupported(error)) return LISTING_BY_FIND_FIRST;
            fail_windows(error);
            return LISTING_FAILED;
        }
        if (LISTING_QUERY_REFUSED(1) || !GetFileInformationByHandle(directory, &info)) {
            error = GetLastError();
            CloseHandle(directory);
            if (windows_query_unsupported(error)) return LISTING_BY_FIND_FIRST;
            fail_windows(error);
            return LISTING_FAILED;
        }
        if ((flags & FILE_FLAG_OPEN_REPARSE_POINT) != 0 &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            CloseHandle(directory);
            flags = FILE_FLAG_BACKUP_SEMANTICS;
            continue;
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            CloseHandle(directory);
            errno = ENOTDIR;
            return LISTING_FAILED;
        }
        *output = directory;
        return LISTING_HANDLE;
    }
}

/* The reparse tag of entry `name` of `directory`, read from the entry itself
 * and never from what it points at; 0 when it cannot be read. A listing
 * entry carries the tag in EaSize ([MS-FSCC] FileIdBothDirectoryInformation,
 * and Windows' own FindFirstFileExW reads it there), but Wine leaves EaSize 0,
 * which is not a valid tag. */
static DWORD listed_reparse_tag(const wchar_t *directory, const wchar_t *name) {
    const size_t length = wcslen(directory);
    wchar_t *child = (wchar_t *)calloc(length + wcslen(name) + 2u, sizeof(*child));
    FILE_ATTRIBUTE_TAG_INFO info;
    HANDLE entry;
    DWORD tag = 0;
    if (child == NULL) return 0;
    wcscpy(child, directory);
    if (length == 0 || child[length - 1] != L'\\') wcscat(child, L"\\");
    wcscat(child, name);
    entry = CreateFileW(child, FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                        NULL);
    free(child);
    if (entry == INVALID_HANDLE_VALUE) return 0;
    if (GetFileInformationByHandleEx(entry, FileAttributeTagInfo, &info, sizeof info) &&
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        tag = info.ReparseTag;
    }
    CloseHandle(entry);
    return tag;
}

/* Report one listed entry; `name` is NUL-terminated. Returns 0 to go on, 1
 * when the listing failed, 2 when the visitor stopped it. */
static int visit_listed_entry(const wchar_t *directory, const wchar_t *name,
                              DWORD attributes, DWORD tag,
                              MdkrDirectoryVisit visit, void *user) {
    const int reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    char *converted = NULL;
    int kind;
    int stopped;
    if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) return 0;
    if (!reparse) tag = 0;
    else if (tag == 0) tag = listed_reparse_tag(directory, name);
    /* A point whose tag cannot be read is a link, never followed, as in
     * mdkr_path_kind_utf8. */
    kind = reparse && tag == 0 ? MDKR_PATH_LINK : windows_entry_kind(attributes, tag);
    if (wide_to_utf8_alloc(name, &converted) != 0) {
        /* Not valid UTF-16 (an unpaired surrogate): report it under a
         * stand-in rather than skip it, so a caller copying a folder can
         * refuse instead of silently leaving the entry out. */
        if (wide_to_utf8_convert(name, &converted, 0) != 0) return 1;
        kind = MDKR_PATH_BAD_NAME;
    }
    stopped = visit(converted, kind, user) != 0;
    free(converted);
    return stopped ? 2 : 0;
}

/* The listing FindFirstFileW gives, for a file system that does not answer
 * the directory query on a handle. Its own handle's sharing mode decides
 * whether a held directory can be listed. */
static int list_with_find_first(const wchar_t *wide, MdkrDirectoryVisit visit,
                                void *user) {
    const size_t length = wcslen(wide);
    wchar_t *pattern = (wchar_t *)calloc(length + 3u, sizeof(*pattern));
    WIN32_FIND_DATAW data;
    HANDLE find;
    int result = 0;
    if (pattern == NULL) return 1;
    wcscpy(pattern, wide);
    if (length == 0 || pattern[length - 1] != L'\\') wcscat(pattern, L"\\");
    wcscat(pattern, L"*");
    find = FindFirstFileW(pattern, &data);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        /* An empty volume root has no "." or ".." to report. */
        if (error == ERROR_FILE_NOT_FOUND) return 0;
        fail_windows(error);
        return 1;
    }
    do {
        result = visit_listed_entry(wide, data.cFileName, data.dwFileAttributes,
                                    data.dwReserved0, visit, user);
    } while (result == 0 && FindNextFileW(find, &data));
    if (result == 0 && GetLastError() != ERROR_NO_MORE_FILES) {
        fail_windows(GetLastError());
        result = 1;
    }
    FindClose(find);
    return result;
}

int mdkr_directory_list_utf8(const char *path, MdkrDirectoryVisit visit,
                             void *user) {
    /* FILE_ID_BOTH_DIR_INFO is DWORD/LARGE_INTEGER aligned; malloc's alignment
     * covers it. One query fills as many whole entries as fit. */
    enum { LIST_BUFFER_BYTES = 64 * 1024 };
    wchar_t *wide;
    wchar_t *name = NULL;
    unsigned char *buffer = NULL;
    HANDLE directory;
    FILE_INFO_BY_HANDLE_CLASS query = FileIdBothDirectoryRestartInfo;
    int result = 0;
    if (visit == NULL) {
        errno = EINVAL;
        return 1;
    }
    wide = utf8_to_extended_path(path);
    if (wide == NULL) return 1;
    switch (open_directory_for_listing(wide, &directory)) {
    case LISTING_HANDLE:
        break;
    case LISTING_BY_FIND_FIRST:
        result = list_with_find_first(wide, visit, user);
        free(wide);
        return result;
    default:
        free(wide);
        return 1;
    }
    buffer = (unsigned char *)malloc(LIST_BUFFER_BYTES);
    name = (wchar_t *)malloc(LIST_BUFFER_BYTES + sizeof(wchar_t));
    if (buffer == NULL || name == NULL) {
        result = 1;
        goto done;
    }
    while (result == 0) {
        const FILE_ID_BOTH_DIR_INFO *entry;
        if (LISTING_QUERY_REFUSED(2) ||
            !GetFileInformationByHandleEx(directory, query, buffer,
                                          LIST_BUFFER_BYTES)) {
            const DWORD error = GetLastError();
            /* The handle proved the directory exists, so "no such file" from
             * the first query is an empty volume root (it has no "." or
             * ".."), not a missing directory. */
            if (error == ERROR_NO_MORE_FILES ||
                (error == ERROR_FILE_NOT_FOUND &&
                 query == FileIdBothDirectoryRestartInfo)) {
                break;
            }
            if (query == FileIdBothDirectoryRestartInfo &&
                windows_query_unsupported(error)) {
                /* Some network and virtual drives do not answer this query
                 * on a handle; nothing has been visited yet. A refusal of a
                 * later query cannot fall back: entries were reported. */
                CloseHandle(directory);
                directory = INVALID_HANDLE_VALUE;
                result = list_with_find_first(wide, visit, user);
                break;
            }
            fail_windows(error);
            result = 1;
            break;
        }
        query = FileIdBothDirectoryInfo;
        entry = (const FILE_ID_BOTH_DIR_INFO *)buffer;
        for (;;) {
            /* FileNameLength is in bytes and the name is not terminated. For
             * a reparse point EaSize holds its reparse tag. */
            const size_t units = entry->FileNameLength / sizeof(WCHAR);
            memcpy(name, entry->FileName, units * sizeof(WCHAR));
            name[units] = L'\0';
            result = visit_listed_entry(wide, name, entry->FileAttributes,
                                        entry->EaSize, visit, user);
            if (result != 0 || entry->NextEntryOffset == 0) break;
            entry = (const FILE_ID_BOTH_DIR_INFO *)((const unsigned char *)entry +
                                                    entry->NextEntryOffset);
        }
    }
done:
    free(buffer);
    free(name);
    free(wide);
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    return result;
}

int mdkr_rename_exclusive_utf8(const char *from, const char *to) {
    wchar_t *wide_from = utf8_to_extended_path(from);
    wchar_t *wide_to = utf8_to_extended_path(to);
    int result = -1;
    if (wide_from != NULL && wide_to != NULL) {
        for (int attempt = 0;; ++attempt) {
            DWORD error;
            /* No MOVEFILE_REPLACE_EXISTING and no MOVEFILE_COPY_ALLOWED: a
             * same-volume rename that fails when the name is taken. */
            if (MoveFileExW(wide_from, wide_to, 0)) {
                result = 0;
                break;
            }
            error = GetLastError();
            /* Security scanners and indexers briefly open freshly written
             * files; a rename inside that window is refused transiently. */
            if ((error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED ||
                 error == ERROR_LOCK_VIOLATION) && attempt < 9) {
                Sleep(50);
                continue;
            }
            fail_windows(error);
            break;
        }
    }
    free(wide_from);
    free(wide_to);
    return result;
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

static wchar_t *plain_wide_argument(const char *argument) {
    wchar_t *wide;
    int count;
    if (argument == NULL || argument[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                argument, -1, NULL, 0);
    if (count <= 0) {
        errno = EINVAL;
        return NULL;
    }
    wide = (wchar_t *)calloc((size_t)count, sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            argument, -1, wide, count) <= 0) {
        free(wide);
        errno = EINVAL;
        return NULL;
    }
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

int mdkr_spawn_wait_utf8(const char *path, const char *const *arguments,
                         int *exit_code) {
    wchar_t *image = plain_wide_argument(path);
    wchar_t **vector = NULL;
    size_t count = 0u;
    size_t index;
    intptr_t status;
    int saved;
    if (exit_code != NULL) *exit_code = -1;
    if (image == NULL) return errno ? errno : EINVAL;
    if (arguments != NULL) while (arguments[count] != NULL) ++count;
    vector = (wchar_t **)calloc(count + 2u, sizeof(*vector));
    if (vector == NULL) {
        saved = errno ? errno : ENOMEM;
        free(image);
        return saved;
    }
    vector[0] = quoted_wide_argument(path);
    for (index = 0u; index < count && vector[index] != NULL; ++index) {
        vector[index + 1u] = quoted_wide_argument(arguments[index]);
    }
    if (vector[count] == NULL) {
        saved = errno ? errno : EINVAL;
        for (index = 0u; index <= count; ++index) free(vector[index]);
        free(vector);
        free(image);
        return saved;
    }
    status = _wspawnvp(_P_WAIT, image, (const wchar_t *const *)vector);
    saved = status < 0 ? (errno ? errno : EIO) : 0;
    if (status >= 0 && exit_code != NULL) *exit_code = (int)status;
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
    /* A directory hold has no byte-range lock; this fails harmlessly there. */
    (void)UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle);
    lock->handle = (intptr_t)-1;
}

int mdkr_directory_hold_utf8(const char *path, MdkrFileLock *hold) {
    wchar_t *wide;
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;
    if (hold == NULL) {
        errno = EINVAL;
        return -1;
    }
    hold->handle = (intptr_t)-1;
    wide = utf8_to_extended_path(path);
    if (wide == NULL) return -1;
    /* DELETE access without FILE_SHARE_DELETE: a second holder, a rename and a
     * delete of this directory all need DELETE access, which this handle's
     * sharing refuses. Listing it and changing its contents do not. */
    handle = CreateFileW(wide, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        fail_windows(GetLastError());
        return -1;
    }
    if (!GetFileInformationByHandle(handle, &info)) {
        fail_windows(GetLastError());
        CloseHandle(handle);
        return -1;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        CloseHandle(handle);
        errno = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? ELOOP : ENOTDIR;
        return -1;
    }
    hold->handle = (intptr_t)handle;
    return 0;
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
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* RENAME_NOREPLACE's value is part of the Linux renameat2 ABI; older C
 * libraries do not declare it. */
#define MDKR_RENAME_NOREPLACE 1

extern char **environ;

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
#ifdef __EMSCRIPTEN__
        /* Emscripten's file system has no hard links: link() always fails
         * (EMLINK), so the install below could never move a file in the
         * browser, and a damaged pak or ghost file was never set aside. One
         * single-threaded engine owns this file system and nothing yields
         * between these two calls, so the check cannot go stale (fs_utf8.h).
         * lstat: a dangling link is a name already taken. */
        struct stat status;
        if (lstat(to, &status) == 0) {
            errno = EEXIST;
            return -1;
        }
        if (errno != ENOENT) return -1;
        return rename(from, to);
#else
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
#endif
    }
    return rename(from, to);
}
int mdkr_path_query_utf8(const char *path, int *exists,
                         int *is_regular, int *is_directory) {
    struct stat status;
    if (exists) *exists = 0;
    if (is_regular) *is_regular = 0;
    if (is_directory) *is_directory = 0;
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &status) != 0) {
        /* fs_utf8.h: missing is an answer, not an error. */
        return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    }
    if (exists) *exists = 1;
    if (is_regular) *is_regular = S_ISREG(status.st_mode);
    if (is_directory) *is_directory = S_ISDIR(status.st_mode);
    return 0;
}
int mdkr_path_is_link_or_reparse_utf8(const char *path) {
    struct stat status;
    if (path == NULL || lstat(path, &status) != 0) return -1;
    return S_ISLNK(status.st_mode) ? 1 : 0;
}
/* 1 when `text` is well-formed UTF-8: no stray continuation byte, no
 * truncated or overlong sequence, no UTF-16 surrogate, nothing past
 * U+10FFFF. */
static int utf8_well_formed(const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != 0u) {
        const size_t sequence = utf8_sequence_length(cursor);
        if (sequence == 0u) return 0;
        cursor += sequence;
    }
    return 1;
}
static int posix_entry_kind(const struct stat *status) {
    if (S_ISLNK(status->st_mode)) return MDKR_PATH_LINK;
    if (S_ISDIR(status->st_mode)) return MDKR_PATH_DIRECTORY;
    if (S_ISREG(status->st_mode)) return MDKR_PATH_FILE;
    return MDKR_PATH_OTHER;
}
int mdkr_path_kind_utf8(const char *path) {
    struct stat status;
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (lstat(path, &status) != 0) {
        return errno == ENOENT || errno == ENOTDIR ? MDKR_PATH_MISSING : -1;
    }
    return posix_entry_kind(&status);
}
int mdkr_directory_list_utf8(const char *path, MdkrDirectoryVisit visit,
                             void *user) {
    DIR *directory;
    int result = 0;
    if (path == NULL || visit == NULL) {
        errno = EINVAL;
        return 1;
    }
    directory = opendir(path);
    if (directory == NULL) return 1;
    while (result == 0) {
        struct dirent *entry;
        struct stat status;
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) result = 1;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (fstatat(dirfd(directory), entry->d_name, &status,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue; /* removed while listing */
            result = 1;
            break;
        }
        /* Every path inside the game is UTF-8: a name that is not has no
         * spelling there, and is reported as such, as on Windows. */
        if (visit(entry->d_name,
                  utf8_well_formed(entry->d_name) ? posix_entry_kind(&status)
                                                  : MDKR_PATH_BAD_NAME,
                  user) != 0) {
            result = 2;
        }
    }
    closedir(directory);
    return result;
}
int mdkr_rename_exclusive_utf8(const char *from, const char *to) {
    if (from == NULL || to == NULL) {
        errno = EINVAL;
        return -1;
    }
#if defined(__APPLE__)
    return renamex_np(from, to, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to,
                        MDKR_RENAME_NOREPLACE);
#else
    errno = ENOTSUP;
    return -1;
#endif
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
int mdkr_spawn_wait_utf8(const char *path, const char *const *arguments,
                         int *exit_code) {
#ifdef __EMSCRIPTEN__
    (void)path;
    (void)arguments;
    if (exit_code != NULL) *exit_code = -1;
    return ENOSYS;
#else
    char **vector;
    size_t count = 0u;
    size_t index;
    pid_t process;
    int status;
    int result;
    if (exit_code != NULL) *exit_code = -1;
    if (path == NULL || path[0] == '\0') return EINVAL;
    if (arguments != NULL) while (arguments[count] != NULL) ++count;
    vector = (char **)calloc(count + 2u, sizeof(*vector));
    if (vector == NULL) return errno ? errno : ENOMEM;
    vector[0] = (char *)path;
    for (index = 0u; index < count; ++index) {
        vector[index + 1u] = (char *)arguments[index];
    }
    result = posix_spawnp(&process, path, NULL, NULL, vector, environ);
    free(vector);
    if (result != 0) return result;
    do {
        result = waitpid(process, &status, 0) < 0 ? errno : 0;
    } while (result == EINTR);
    if (result != 0) return result;
    if (exit_code != NULL) {
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    }
    return 0;
#endif
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
int mdkr_directory_hold_utf8(const char *path, MdkrFileLock *hold) {
    int descriptor;
    if (hold == NULL) {
        errno = EINVAL;
        return -1;
    }
    hold->handle = -1;
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    descriptor = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return -1;
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno == EWOULDBLOCK ? EBUSY : errno;
        close(descriptor);
        errno = saved;
        return -1;
    }
    hold->handle = (intptr_t)descriptor;
    return 0;
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

/* ---- Collected listings (fs_utf8.h), the same code on every platform ---- */

#ifdef MDKR_FS_UTF8_TESTING
/* Test-only, for the tests that define MDKR_FS_UTF8_TESTING: at 0 or more, a
 * collected listing stops with EIO once it holds that many names, as a read
 * error part-way through a listing would. */
int mdkr_fs_utf8_test_names_fail_after = -1;
#define NAMES_LISTING_FAILS(collected) \
    (mdkr_fs_utf8_test_names_fail_after >= 0 && \
     (collected) >= (size_t)mdkr_fs_utf8_test_names_fail_after)
#else
#define NAMES_LISTING_FAILS(collected) 0
#endif

/* A readable copy of a name with no UTF-8 spelling: what is well-formed is
 * kept and every other byte becomes U+FFFD. (Windows' stand-in is already
 * converted that way, so it comes back unchanged.) Never a path. */
static char *utf8_display_stand_in(const char *name) {
    const unsigned char *cursor = (const unsigned char *)name;
    const size_t length = strlen(name);
    char *output;
    size_t written = 0u;
    if (length > (SIZE_MAX - 1u) / 3u) {
        errno = ENOMEM;
        return NULL;
    }
    output = (char *)malloc(length * 3u + 1u);
    if (output == NULL) return NULL;
    while (*cursor != 0u) {
        const size_t sequence = utf8_sequence_length(cursor);
        if (sequence == 0u) {
            memcpy(output + written, "\xEF\xBF\xBD", 3u);
            written += 3u;
            ++cursor;
        } else {
            memcpy(output + written, cursor, sequence);
            written += sequence;
            cursor += sequence;
        }
    }
    output[written] = '\0';
    return output;
}

typedef struct DirectoryNamesCollector {
    MdkrDirectoryNames *names;
    size_t names_capacity;
    size_t unnamed_capacity;
    int failed;
} DirectoryNamesCollector;

/* Append `entry` (owned) to `*list`, growing it. Returns 0, or -1 when memory
 * ran out (the entry is then released). */
static int names_append(char ***list, size_t *count, size_t *capacity,
                        char *entry) {
    if (*count == *capacity) {
        const size_t grown_capacity = *capacity == 0u ? 16u : *capacity * 2u;
        char **grown;
        if (grown_capacity < *capacity ||
            grown_capacity > SIZE_MAX / sizeof(*grown)) {
            free(entry);
            errno = ENOMEM;
            return -1;
        }
        grown = (char **)realloc(*list, grown_capacity * sizeof(*grown));
        if (grown == NULL) {
            free(entry);
            return -1;
        }
        *list = grown;
        *capacity = grown_capacity;
    }
    (*list)[(*count)++] = entry;
    return 0;
}

static int collect_directory_name(const char *name, int kind, void *user) {
    DirectoryNamesCollector *collector = (DirectoryNamesCollector *)user;
    MdkrDirectoryNames *names = collector->names;
    char *copy;
    int appended;
    if (NAMES_LISTING_FAILS(names->count + names->unnamed_count)) {
        errno = EIO;
        collector->failed = 1;
        return 1;
    }
    if (kind == MDKR_PATH_BAD_NAME) {
        copy = utf8_display_stand_in(name);
    } else {
        const size_t length = strlen(name);
        copy = (char *)malloc(length + 1u);
        if (copy != NULL) memcpy(copy, name, length + 1u);
    }
    if (copy == NULL) {
        collector->failed = 1;
        return 1;
    }
    appended = kind == MDKR_PATH_BAD_NAME
        ? names_append(&names->unnamed, &names->unnamed_count,
                       &collector->unnamed_capacity, copy)
        : names_append(&names->names, &names->count,
                       &collector->names_capacity, copy);
    if (appended != 0) {
        collector->failed = 1;
        return 1;
    }
    return 0;
}

int mdkr_directory_names_utf8(const char *path, MdkrDirectoryNames *names) {
    DirectoryNamesCollector collector;
    if (names == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(names, 0, sizeof(*names));
    memset(&collector, 0, sizeof(collector));
    collector.names = names;
    errno = 0;
    if (mdkr_directory_list_utf8(path, collect_directory_name, &collector) != 0 ||
        collector.failed) {
        /* What was read before the failure stays held (fs_utf8.h). */
        if (errno == 0) errno = EIO;
        return -1;
    }
    return 0;
}

void mdkr_directory_names_free(MdkrDirectoryNames *names) {
    size_t index;
    if (names == NULL) return;
    for (index = 0u; index < names->count; ++index) free(names->names[index]);
    for (index = 0u; index < names->unnamed_count; ++index) {
        free(names->unnamed[index]);
    }
    free(names->names);
    free(names->unnamed);
    memset(names, 0, sizeof(*names));
}
