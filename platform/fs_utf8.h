/**
 * UTF-8 filesystem/process boundary for native builds.
 *
 * Every path inside Golden Balloon is UTF-8. POSIX consumes that encoding
 * directly. Windows' narrow CRT/Win32 entry points consume the active code
 * page instead, so a perfectly valid path returned by GetOpenFileNameW used to
 * fail when it contained non-ASCII text. These wrappers convert once at the OS
 * boundary and add the extended-length prefix to absolute Windows paths.
 */
#ifndef MDKR_FS_UTF8_H
#define MDKR_FS_UTF8_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *mdkr_fopen_utf8(const char *path, const char *mode);
int mdkr_remove_utf8(const char *path);
int mdkr_unlink_utf8(const char *path);
int mdkr_mkdir_utf8(const char *path);
int mdkr_rmdir_utf8(const char *path);

/** Atomic move. `replace` permits replacing a file; `write_through` requests
 * durable metadata completion on Windows. Returns zero on success. */
int mdkr_move_utf8(const char *from, const char *to,
                   int replace, int write_through);

/** Query `path` without opening it. Any output pointer may be NULL; every
 * output is cleared first. POSIX follows a final link (so a dangling link is
 * missing); Windows answers for the entry itself.
 *
 * One contract on every platform:
 *   - 0 with *exists = 1: the path exists, and *is_regular / *is_directory
 *     say what it is.
 *   - 0 with *exists = 0: the path is MISSING. The name, or a folder above
 *     it, does not exist, or a folder above it is a file: ENOENT or ENOTDIR
 *     on POSIX; ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_DIRECTORY
 *     or ERROR_INVALID_DRIVE on Windows. A caller that needs the path
 *     creates it.
 *   - -1 with errno set: a REAL ERROR. Permission denied, a name the file
 *     system refuses, a NULL or empty path, an I/O failure. The path may
 *     exist, so a caller reports the failure and never creates anything.
 *
 * (Before 1.8, POSIX answered a missing path with -1 and Windows answered
 * every failure with 0, so a caller written against one platform broke on
 * the other: a missing save folder was never created on Windows.) */
int mdkr_path_query_utf8(const char *path, int *exists,
                         int *is_regular, int *is_directory);

/** Return 1 when `path` itself is a POSIX symbolic link or Windows reparse
 * point, 0 for an ordinary entry, and -1 when it cannot be queried. */
int mdkr_path_is_link_or_reparse_utf8(const char *path);

/* ---- Pathname primitives for reading and installing content packs ----
 *
 * Windows reads and installs packs by pathname through these calls. POSIX
 * production code works relative to pinned directory descriptors instead and
 * runs these only in the tests that execute the pathname logic on POSIX. */
typedef enum MdkrPathKind {
    MDKR_PATH_MISSING = 0,
    MDKR_PATH_FILE = 1,
    MDKR_PATH_DIRECTORY = 2,
    /* A POSIX symbolic link, or a Windows name-surrogate reparse point
     * (symbolic link, junction, mount point). Never followed. Other Windows
     * reparse points -- cloud-file placeholders, deduplicated files -- report
     * the kind of data they hold. */
    MDKR_PATH_LINK = 3,
    /* FIFO, socket, device. */
    MDKR_PATH_OTHER = 4,
    /* Listings only: an entry whose name has no UTF-8 spelling -- on Windows
     * a name that is not valid UTF-16 (an unpaired surrogate), elsewhere one
     * that is not valid UTF-8. The name handed to the visitor is a stand-in
     * (Windows replaces what cannot convert with U+FFFD; elsewhere the bytes
     * are passed as they are) and must never be used to reach the entry. */
    MDKR_PATH_BAD_NAME = 5
} MdkrPathKind;

/** What `path` itself is, never following a final link. Returns an
 * MdkrPathKind, or -1 with errno set when the entry exists but could not be
 * queried. A missing ancestor or an unusable name reports MDKR_PATH_MISSING. */
int mdkr_path_kind_utf8(const char *path);

/** Visit every entry of directory `path` except "." and "..", with its kind as
 * mdkr_path_kind_utf8 reports it. `path` itself may be reached through a link;
 * the entries are never followed. A non-zero visitor return stops the listing.
 * A name that is not valid Unicode is not skipped: it is reported as
 * MDKR_PATH_BAD_NAME under a stand-in name. Returns 0 when the listing
 * completed, 1 when the directory could not be listed, 2 when the visitor
 * stopped it. */
typedef int (*MdkrDirectoryVisit)(const char *name, int kind, void *user);
int mdkr_directory_list_utf8(const char *path, MdkrDirectoryVisit visit,
                             void *user);

/** The entry names of directory `path`, as mdkr_directory_list_utf8 lists
 * them, collected first so a caller may change the directory while it works
 * through them. This is how product code lists a folder: never narrow
 * opendir()/readdir(), which on Windows read paths and names in the ANSI code
 * page and cannot list a folder (or return a name) outside it. A name with no
 * UTF-8 spelling (MDKR_PATH_BAD_NAME) cannot reach its entry, so it is left
 * out and counted in `unnamed`. Returns 0 with `names` filled (release it with
 * mdkr_directory_names_free), or -1 with errno set and nothing held when the
 * directory could not be listed completely. A missing directory is -1 here:
 * ask mdkr_path_query_utf8 whether that failure means "missing". */
typedef struct MdkrDirectoryNames {
    char **names;
    size_t count;
    size_t unnamed;
} MdkrDirectoryNames;
int mdkr_directory_names_utf8(const char *path, MdkrDirectoryNames *names);
void mdkr_directory_names_free(MdkrDirectoryNames *names);

/** Rename a file or directory within one volume, refusing with EEXIST to
 * replace anything already named `to` (a dangling link included). There is
 * no check-then-rename fallback: where the filesystem cannot refuse
 * replacement atomically the call fails (ENOTSUP, EOPNOTSUPP or EINVAL).
 * Returns 0, or -1 with errno set. */
int mdkr_rename_exclusive_utf8(const char *from, const char *to);

/** Replace this process with `path`, passing the NULL-terminated UTF-8
 * `arguments` after argv[0] (pass NULL for none). Returns errno only when
 * replacement fails.
 *
 * The replacement receives exactly the arguments given here on every platform.
 * That is not free on Windows: the CRT's _exec/_spawn family joins argv with
 * single spaces and performs NO quoting of its own, so an install path
 * containing a space (C:\Program Files\..., C:\Users\First Last\...) used to
 * arrive at the replacement split across several arguments. This wrapper
 * applies the Windows quoting rule below before handing argv to the CRT. */
int mdkr_exec_replace_utf8(const char *path, const char *const *arguments);

/** Start a child process without a shell and wait for it. `arguments` excludes
 * argv[0], exactly like mdkr_exec_replace_utf8. Returns zero when the child was
 * launched and reaped (its actual status is written to `exit_code`), or an
 * errno-style value when it could not be started/waited. */
int mdkr_spawn_wait_utf8(const char *path, const char *const *arguments,
                         int *exit_code);

/** Quote one argument so that the Windows command-line parser
 * (CommandLineToArgvW and the CRT's own argv construction) recovers it
 * verbatim: the whole argument is wrapped in quotes, embedded quotes are
 * escaped, and each backslash run that precedes a quote is doubled.
 *
 * Compiled on every platform even though only the Windows arm consumes it, so
 * the rule itself stays under test wherever the suite runs. Writes a NUL
 * terminator and returns the length excluding it, or -1 when `argument` is
 * NULL or `capacity` cannot hold the result. */
int mdkr_windows_quote_argument_utf8(const char *argument, char *output,
                                     size_t capacity);

/* Flush file contents and, on POSIX, the directory entry containing `path`.
 * Call both sides of a replace transaction before reporting durable success. */
int mdkr_file_sync(FILE *file);
int mdkr_parent_directory_sync_utf8(const char *path);

/* An advisory, process-safe lock held on a sidecar file. The operating system
 * releases it if the process terminates, so a crash cannot leave a stale lock. */
typedef struct MdkrFileLock {
    intptr_t handle;
} MdkrFileLock;
int mdkr_file_lock_acquire_utf8(const char *path, MdkrFileLock *lock);
void mdkr_file_lock_release(MdkrFileLock *lock);

/** Hold a directory as a live transaction's workspace (see the pathname
 * primitives above). While it is held, a second hold on the same directory,
 * from any process, fails with EBUSY; the operating system drops the hold
 * when the process ends, so a crash leaves nothing held. Windows also refuses
 * to rename or delete a held directory, while its contents stay freely
 * writable. Refuses a link or non-directory. Release with
 * mdkr_file_lock_release(). Returns 0, or -1 with errno set. */
int mdkr_directory_hold_utf8(const char *path, MdkrFileLock *hold);

/* Resolve the executable that is actually running, rather than trusting a
 * potentially PATH-relative argv[0]: GetModuleFileNameW on Windows,
 * _NSGetExecutablePath on macOS, /proc/self/exe on Linux, and ENOSYS where the
 * operating system offers no such query. On success this allocates `*output`,
 * which the caller must free. There is deliberately no caller-sized buffer:
 * a Windows executable path may need up to 32,767 UTF-16 units and up to four
 * UTF-8 bytes per unit. */
int mdkr_running_executable_path_utf8(char **output);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_FS_UTF8_H */
