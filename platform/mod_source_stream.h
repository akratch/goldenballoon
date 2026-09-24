#ifndef MDKR64_MOD_SOURCE_STREAM_H
#define MDKR64_MOD_SOURCE_STREAM_H

/* Keep libc FILE separate from the core header: legacy game declarations are
 * sensitive to platform stdio macro/include order in optimized native builds. */
#include "mod_source.h"
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Owned binary stream for a regular file beneath a pinned DIRECTORY source.
 * Uses the same path validation and no-follow component walk as buffered reads.
 * ZIP sources are refused. Caller must fclose(), bound bytes consumed and check
 * I/O errors; unlike read(), this primitive does not enforce entry size.
 * Intended for cancellable installer copies without whole-file allocation. */
FILE *mdkr_mod_source_open_directory_file(MdkrModSource *src, const char *rel);

/* Consume an already-open ZIP stream on success AND failure. The source owns
 * it until close. Allows validation of staged bytes without reopening a path. */
MdkrModSource *mdkr_mod_source_from_zip_stream(FILE *stream);
#ifndef _WIN32
/* Borrow a POSIX directory descriptor and open an independent owned reference.
 * Caller retains its descriptor. No pathname is consulted. */
MdkrModSource *mdkr_mod_source_from_directory_fd(int fd);
#else
/* Borrow a Windows directory handle and retain an independently opened source.
 * No pathname is consulted; caller retains ownership of its handle. */
MdkrModSource *mdkr_mod_source_from_directory_handle(intptr_t handle);
#endif

#ifdef __cplusplus
}
#endif
#endif
