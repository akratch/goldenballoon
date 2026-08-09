/* mod_texture_store.h — decoded pack textures, keyed by content digest.
 *
 * The renderer asks this store one question, once per texture it is about to
 * upload: "does an installed pack supply this picture?". Everything else here
 * exists to make that question cheap enough to ask on a hot path and safe
 * enough to answer from files a stranger authored.
 *
 * Three properties the renderer relies on:
 *
 *   Inert until installed. With no registry, no packs, or overrides switched
 *   off, mdkr_mod_texture_lookup() returns 0 without touching the filesystem,
 *   and mdkr_mod_texture_store_active() reports so before the caller has even
 *   paid for a digest. A build with no mods/ directory must render exactly the
 *   bytes it rendered before this module existed.
 *
 *   Lazy and bounded. A pack's PNGs are decoded on first use, not at startup —
 *   a large pack would otherwise add seconds to launch for textures the player
 *   may never see. Decoded pixels are capped in total and the least recently
 *   used are dropped past the cap, so a 4K pack cannot quietly exhaust memory.
 *
 *   Quiet about the same failure twice. A PNG that will not decode is reported
 *   once, with its digest and the decoder's own reason, and is thereafter
 *   treated as absent. Re-reporting it would put a line in the log every frame
 *   the texture is bound.
 *
 * Single-threaded by standing decision (docs/ARCHITECTURE_DECISIONS.md §1: the
 * port is cooperative, with no real threads). There is deliberately no lock and
 * no atomic here; if that decision is ever revisited, this module is one of the
 * places that has to be revisited with it.
 */
#ifndef MDKR64_MOD_TEXTURE_STORE_H
#define MDKR64_MOD_TEXTURE_STORE_H

#include "mod_registry.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrModTexture {
    /* Store-owned RGBA8, tightly packed, `width * height * 4` bytes.
     *
     * Valid until the next mdkr_mod_texture_lookup() or
     * mdkr_mod_texture_store_shutdown(): a later lookup may evict these pixels
     * to stay under the cache cap. Every caller so far consumes them
     * immediately (uploads them and forgets them), which is the usage this
     * contract is written for — do not retain the pointer across a lookup. */
    const uint8_t *rgba;
    int width, height;
} MdkrModTexture;

/* Binds the store to an already-scanned registry. `registry` is borrowed, not
 * copied: it must outlive the store, and a rescan means shutdown then init
 * again. Passing NULL is legal and leaves the store permanently inactive. */
void mdkr_mod_texture_store_init(const MdkrModRegistry *registry);
/* Frees every decoded texture and unbinds the registry. Safe to call twice,
 * and safe to call when init never ran. */
void mdkr_mod_texture_store_shutdown(void);

/* 1 and fills `out` when an enabled pack provides `digest_hex`; 0 otherwise.
 * Returns 0 unconditionally while overrides are disabled. `*out` is cleared on
 * every path that returns 0, so a caller that forgets to check the return value
 * reads a null pointer rather than a stale texture. */
int  mdkr_mod_texture_lookup(const char *digest_hex, MdkrModTexture *out);

/* True when a lookup could possibly succeed: overrides on, a registry bound,
 * and at least one enabled pack in it. The renderer tests this before hashing
 * a texture, because computing a digest for a store that cannot answer is the
 * one cost this feature would otherwise impose on every install that has no
 * packs at all. */
bool mdkr_mod_texture_store_active(void);

void     mdkr_mod_texture_set_enabled(bool enabled);
bool     mdkr_mod_texture_enabled(void);
/* Increments on every enable/disable. Feeds DkrTexCacheKey.override_generation,
 * which is what makes the two variants of a texture distinct cache entries
 * rather than one entry that gets mutated under a live binding. Called once per
 * texture bind, so it is a plain read and nothing more. */
uint32_t mdkr_mod_texture_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_TEXTURE_STORE_H */
