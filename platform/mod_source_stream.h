#ifndef MDKR64_MOD_SOURCE_STREAM_H
#define MDKR64_MOD_SOURCE_STREAM_H

/* Keep libc FILE separate from the core header: legacy game declarations are
 * sensitive to platform stdio macro/include order in optimized native builds. */
#include "mod_source.h"
#include <stdio.h>
#include <stdint.h>

/* Folder packs are read by pathname on Windows and relative to pinned
 * directory descriptors elsewhere. Defining MDKR_PACK_PATH_ARM selects the
 * pathname arm on a POSIX host, so the tests of the Windows logic run where
 * the suite runs. */
#if defined(_WIN32) || defined(MDKR_PACK_PATH_ARM)
#define MDKR_PACK_BY_PATH 1
#else
#define MDKR_PACK_BY_PATH 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Owned binary stream for a regular file beneath a DIRECTORY source. Uses the
 * same path validation and link refusal as buffered reads. ZIP sources are
 * refused. On failure errno is ENOENT for an absent file and ELOOP for a link
 * below the root. Caller must fclose(), bound bytes consumed and check I/O
 * errors; unlike read(), this primitive does not enforce entry size.
 * Intended for cancellable installer copies without whole-file allocation. */
FILE *mdkr_mod_source_open_directory_file(MdkrModSource *src, const char *rel);

/* Consume an already-open ZIP stream on success AND failure. The source owns
 * it until close. Allows validation of staged bytes without reopening a path.
 * The archive is measured with 64-bit offsets, so one over 2 GiB opens. */
MdkrModSource *mdkr_mod_source_from_zip_stream(FILE *stream);
#if !MDKR_PACK_BY_PATH
/* Borrow a POSIX directory descriptor and open an independent owned reference.
 * Caller retains its descriptor. No pathname is consulted. */
MdkrModSource *mdkr_mod_source_from_directory_fd(int fd);
#endif

#ifdef __cplusplus
}
#endif
#endif
