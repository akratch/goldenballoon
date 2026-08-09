/* mod_source.h — one read-only interface over the two shapes a content pack
 * can take.
 *
 * A player either unzips a pack into `mods/MyPack/` or drops `MyPack.zip`
 * there and leaves it alone. Both are the same pack, and nothing above this
 * file should have to know which one it got: `mdkr_mod_source_open()` takes
 * the flag the registry already records and everything after it reads the same
 * way. That is not a convenience. Two code paths that fetch pack bytes are two
 * code paths that can disagree, and the way they disagree in practice is that
 * one of them forgets to check the name it was handed.
 *
 * So the name check lives in exactly one function, above the branch, and
 * neither the directory arm nor the archive arm is reachable without passing
 * through it. Every relative path here is attacker-controlled: it comes from a
 * pack a stranger authored, and in the archive case the name is stored inside
 * the archive where the author could put anything at all. A future fix to
 * `mdkr_mod_source_path_is_safe()` therefore lands on both kinds at once,
 * which is the only property of this module worth defending.
 *
 * The second thing an archive can lie about is size. A central directory entry
 * declaring four gigabytes costs a few dozen bytes to write, so no allocation
 * and no loop is ever sized from a declared length before that length has been
 * checked against `MDKR_MOD_SOURCE_ENTRY_MAX`. The same cap applies to files
 * in a directory pack, because a rule the two kinds do not share is a rule
 * that will eventually only be enforced on one of them.
 */
#ifndef MDKR64_MOD_SOURCE_H
#define MDKR64_MOD_SOURCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest single file a pack may serve, 256 MiB. Far above any legitimate
 * texture or audio replacement and far below anything that threatens the
 * process, which is the whole job of the number. It bounds the declared size
 * of an archive entry as well as the on-disk size of a loose file. */
#define MDKR_MOD_SOURCE_ENTRY_MAX (256u * 1024u * 1024u)

/* Every way a lookup can end. "Absent" is deliberately not an error: the
 * common case by far is a pack that simply does not override the file being
 * asked for, and a caller that cannot tell that apart from a broken archive
 * will either log noise on every lookup or swallow real failures. */
typedef enum MdkrModSourceResult {
    MDKR_MOD_SOURCE_OK = 0,
    /* This source does not hold that name. Not a failure. */
    MDKR_MOD_SOURCE_ABSENT = 1,
    /* The name itself is unusable — it escapes the pack, is absolute, or
     * carries separators this port refuses. Nothing was looked up. */
    MDKR_MOD_SOURCE_REJECTED = 2,
    /* The entry exists but is larger than MDKR_MOD_SOURCE_ENTRY_MAX. */
    MDKR_MOD_SOURCE_TOO_LARGE = 3,
    /* The entry exists and fits the cap, but not the buffer offered. */
    MDKR_MOD_SOURCE_BUFFER_TOO_SMALL = 4,
    /* The container or the file underneath it could not be read. */
    MDKR_MOD_SOURCE_IO_ERROR = 5
} MdkrModSourceResult;

typedef struct MdkrModSource MdkrModSource;

/* Opens a pack root. `is_zip` selects the archive arm and matches the flag
 * `MdkrModEntry` already carries, so the registry can pass its own field
 * straight through. Returns NULL when the directory is not a directory, the
 * archive cannot be opened, or its central directory does not parse — a
 * damaged pack disables itself and never takes the process with it.
 *
 * The archive is opened through `mdkr_fopen_utf8()` and handed to miniz as an
 * already-open FILE. miniz's own path-taking entry points go through the
 * narrow CRT, which on Windows would be a second UTF-8 boundary with different
 * behaviour from the one this port already has; there is exactly one such
 * boundary and it stays in fs_utf8. */
MdkrModSource *mdkr_mod_source_open(const char *path, int is_zip);

/* 1 when `rel` names something this source can actually serve. Exactly
 * equivalent to `mdkr_mod_source_read()` returning MDKR_MOD_SOURCE_OK given a
 * large enough buffer, so an entry over the cap answers 0 here and explains
 * itself through read(). */
int mdkr_mod_source_has(MdkrModSource *src, const char *rel);

/* Reads `rel` into `buf`. `rel` always uses '/' separators, on every platform.
 * Returns an MdkrModSourceResult.
 *
 * `out_len` may be NULL. On success it receives the byte count written. On
 * MDKR_MOD_SOURCE_BUFFER_TOO_SMALL it receives the size the caller needs, so a
 * two-pass caller can size its buffer; passing a NULL buffer is a zero
 * capacity and is the supported way to ask. On every other result it is zero —
 * including MDKR_MOD_SOURCE_TOO_LARGE, where reporting the declared size would
 * hand a hostile archive the allocation it was fishing for.
 *
 * Nothing is written into `buf` unless the whole entry fits: a short read that
 * silently truncates a texture is a corrupted asset that looks like a decode
 * bug somewhere else entirely. */
int mdkr_mod_source_read(MdkrModSource *src, const char *rel,
                         void *buf, size_t cap, size_t *out_len);

void mdkr_mod_source_close(MdkrModSource *src);

/* THE path validator, exposed because it is the one thing here that must never
 * be reimplemented. Accepts a non-empty relative path of '/'-separated
 * components; rejects a leading separator, any '\\' or ':' anywhere, an empty
 * or '.' or '..' component, and anything too long to join. Returns 1 when the
 * name is safe to resolve against a pack root. */
int mdkr_mod_source_path_is_safe(const char *rel);

/* A one-line reason for a result code, for the skip and log tables that tell
 * a player why their pack did not do anything. Never NULL. */
const char *mdkr_mod_source_result_text(int result);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_SOURCE_H */
