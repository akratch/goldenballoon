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

/** Query without opening. Any output pointer may be NULL. */
int mdkr_path_query_utf8(const char *path, int *exists,
                         int *is_regular, int *is_directory);

/** Return 1 when `path` itself is a POSIX symbolic link or Windows reparse
 * point, 0 for an ordinary entry, and -1 when it cannot be queried. */
int mdkr_path_is_link_or_reparse_utf8(const char *path);

/** Replace this process with `path` and no extra arguments. Returns errno only
 * when replacement fails. */
int mdkr_exec_replace_utf8(const char *path);

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

/* Resolve the executable that is actually running, rather than trusting a
 * potentially PATH-relative argv[0]. On success this allocates `*output`,
 * which the caller must free. There is deliberately no caller-sized buffer:
 * a Windows executable path may need up to 32,767 UTF-16 units and up to four
 * UTF-8 bytes per unit. */
int mdkr_running_executable_path_utf8(char **output);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_FS_UTF8_H */
