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
 * A separate, additive concern lives here too: with MDKR_MOD_TEXTURE_DUMP set,
 * mdkr_mod_texture_dump_observe() writes the digest-named PNG corpus
 * tools/mod_texture_dump.py's whole job depends on. It answers "what did the
 * game just draw", not "does a pack cover this", which is why it is a
 * question the renderer asks in addition to a lookup rather than a mode this
 * store's lookup path enters -- see mdkr_mod_texture_dump_active() below for
 * why it does not fold into mdkr_mod_texture_store_active().
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

/* ---------------------------------------------- author dump (Task 9) ---- */

/* True when MDKR_MOD_TEXTURE_DUMP names a directory. Independent of
 * mdkr_mod_texture_store_active(): a pack author dumping the digest corpus
 * has no pack installed, so "could an override be found" is false while "the
 * renderer should still resolve a digest and hand it to the store" is true.
 * The renderer ORs the two at its one call site rather than folding this into
 * mdkr_mod_texture_store_active() itself, because that function's contract --
 * "an override could be found" -- is relied on elsewhere (the Content
 * diagnostics line) as a proxy for "is a pack installed", and dump mode alone
 * must not flip that answer. */
bool mdkr_mod_texture_dump_active(void);

/* Records the texture the renderer just resolved for `digest_hex`, for
 * tools/mod_texture_dump.py. `rgba` (tightly packed, `width * height * 4`
 * bytes) must be the pixels the renderer is about to upload for this bind --
 * an installed pack's override when one applied, the ROM decode otherwise --
 * so the dumped PNG is what the game actually displays, not a re-decode this
 * module invents on its own. Writes <dir>/<digest_hex>.png and
 * <digest_hex>.txt (width, height, fmt, siz, first_seen) the first time this
 * digest is observed in the process, and is a no-op on every call after.
 * Unconditionally a no-op unless MDKR_MOD_TEXTURE_DUMP is set: that is the
 * only cost this feature may impose when the variable is absent. */
void mdkr_mod_texture_dump_observe(const char *digest_hex, const uint8_t *rgba,
                                   int width, int height, uint8_t fmt,
                                   uint8_t siz, const char *first_seen);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_TEXTURE_STORE_H */
