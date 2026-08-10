/* mod_manifest.h — one content pack's `pack.ini`, parsed and validated.
 *
 * Pure: no filesystem, no allocation. The caller reads the file; this decides
 * whether its contents describe a loadable pack. A rejected manifest disables
 * exactly one pack and never aborts startup, so every failure path here must
 * produce a human-readable reason rather than a return code alone.
 */
#ifndef MDKR64_MOD_MANIFEST_H
#define MDKR64_MOD_MANIFEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MOD_NAME_MAX    64
#define MDKR_MOD_AUTHOR_MAX  64
#define MDKR_MOD_VERSION_MAX 32

typedef struct MdkrModManifest {
    char name[MDKR_MOD_NAME_MAX];
    char author[MDKR_MOD_AUTHOR_MAX];
    char version[MDKR_MOD_VERSION_MAX];
    /* Ascending load order. Later-loaded packs win a path collision.
     * 0..9999; defaults to 100 so authors can sit either side of the default. */
    int  priority;
    /* 1 unless the manifest says otherwise. The player's own disable list is
     * separate and lives in Content.PackDisabled, not here. */
    int  enabled;
} MdkrModManifest;

/* Returns 0 on success. On failure returns non-zero, writes a one-line reason
 * into `err`, and leaves `*out` unspecified. `ini_text` need not be
 * NUL-terminated; `len` is authoritative. */
int mdkr_mod_manifest_parse(const char *ini_text, size_t len,
                            MdkrModManifest *out, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_MANIFEST_H */
