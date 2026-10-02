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
 *   Off the frame (issue #66). Finding a file is a lookup in the registry's
 *   name index, never a file open. Reading and decoding it happens on one
 *   background worker thread (or, where there are no threads, on the owner
 *   thread under a per-frame time budget), so the first lookup of a texture
 *   answers "not yet" with `pending` set and the renderer draws the original
 *   picture until the replacement is installed by mdkr_mod_texture_store_pump().
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
 * Single-owner by standing decision (docs/ARCHITECTURE_DECISIONS.md §1: the
 * port is cooperative). Every function here is called on the one thread that
 * runs the game and the renderer, and every piece of store state -- the slot
 * table, the LRU, eviction, the counters -- belongs to that thread. The
 * background worker is a host helper in the way the speech worker is: it owns
 * its own open pack handles, reads the one request it was handed, writes that
 * request's result and the pixel buffer it allocated, and meets the owner only
 * at a mutex-guarded hand-off queue in mod_texture_store.c. It never touches
 * the table, the registry, or any other state declared here.
 */
#ifndef MDKR64_MOD_TEXTURE_STORE_H
#define MDKR64_MOD_TEXTURE_STORE_H

#include "mod_registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Generated first-party artwork can change without invalidating existing
 * packs. These are content digests, not a change to the v1 digest algorithm. */
#define MDKR_MOD_TAJ_PORTRAIT_DIGEST "dcd45f4f32c9e1da4abeb3c1c1f8011b"
#define MDKR_MOD_TAJ_PORTRAIT_LEGACY_DIGEST "7757ffb6d3f809fbde246ca559d51eb4"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrModTexture {
    /* Store-owned RGBA8, tightly packed, `width * height * 4` bytes.
     *
     * Valid until the next mdkr_mod_texture_lookup(), pump or
     * mdkr_mod_texture_store_shutdown(): a later install may evict these
     * pixels to stay under the cache cap. Every caller so far consumes them
     * immediately (uploads them and forgets them), which is the usage this
     * contract is written for — do not retain the pointer across a lookup. */
    const uint8_t *rgba;
    int width, height;
    /* 1 when the lookup returned 0 only because a pack DOES supply this
     * texture and it is still being read and decoded: draw the original now,
     * and ask again once mdkr_mod_texture_settled_serial() has moved. 0 on a
     * found texture and on every final "no". */
    int pending;
} MdkrModTexture;

/* Where a replacement is read and decoded. */
typedef enum MdkrModTextureLoader {
    /* Native: WORKER. Browser builds, or a thread that could not be started:
     * BUDGET. MDKR_MOD_TEXTURE_DUMP set: INLINE, because the author dump
     * records the pixels a texture is FIRST drawn with. Otherwise
     * MDKR_TEXTURE_PACK_LOADER=worker|budget|inline picks, for A/B work. */
    MDKR_MOD_TEXTURE_LOADER_AUTO = 0,
    /* One background thread with its own open pack handles. */
    MDKR_MOD_TEXTURE_LOADER_WORKER,
    /* The owner thread, inside mdkr_mod_texture_store_pump(), within the
     * pump's time budget. */
    MDKR_MOD_TEXTURE_LOADER_BUDGET,
    /* The owner thread, inside the lookup itself: the 1.7 behaviour, minus
     * the per-lookup pack opens. Never pending. */
    MDKR_MOD_TEXTURE_LOADER_INLINE
} MdkrModTextureLoader;

/* Asks for a loader at the next mdkr_mod_texture_store_init(). AUTO is the
 * default and what the game uses; tools and tests choose explicitly. */
void mdkr_mod_texture_store_set_loader(MdkrModTextureLoader loader);
/* The loader the bound store is actually using: WORKER, BUDGET or INLINE, or
 * AUTO when the store holds no pack that can supply a texture. */
MdkrModTextureLoader mdkr_mod_texture_store_loader(void);

/* What one pump may spend. Installing a texture costs the renderer one upload
 * of it (about 0.6-0.7 ms per MiB on the hosts measured for issue #66), so the
 * install budget is what bounds that work per frame. At least one completion
 * is installed per pump however large, so nothing can starve. */
typedef struct MdkrModTexturePumpBudget {
    uint32_t install_count;  /* completions installed per pump */
    size_t   install_bytes;  /* RGBA bytes installed per pump */
    uint64_t work_ns;        /* BUDGET loader: read+decode time per pump */
} MdkrModTexturePumpBudget;
#define MDKR_MOD_TEXTURE_INSTALL_COUNT 16u
#define MDKR_MOD_TEXTURE_INSTALL_BYTES ((size_t)4u * 1024u * 1024u)
#define MDKR_MOD_TEXTURE_WORK_NS       ((uint64_t)3000000u)

/* Monotonic nanoseconds, callable from any thread. */
typedef uint64_t (*MdkrModTextureClock)(void);

/* Once per frame, on the owner thread: installs finished replacements within
 * `budget` (NULL: the defaults above) and keeps the loader fed. Under the
 * BUDGET loader it also reads and decodes queued textures until `clock` says
 * the budget's work_ns is spent; with a NULL clock it does one per pump. A
 * no-op with no texture pack bound. */
void mdkr_mod_texture_store_pump(const MdkrModTexturePumpBudget *budget,
                                 MdkrModTextureClock clock);

/* Blocks until nothing is pending: every queued texture read, decoded and
 * installed, budgets ignored. For tests and offline tools -- never per frame. */
void mdkr_mod_texture_store_settle(void);

/* Moves whenever any pending texture settles (installed, refused, found
 * absent, or discarded). A caller holding a "pending" answer re-asks only
 * when this has moved since it was told. */
uint32_t mdkr_mod_texture_settled_serial(void);

/* Called on the owner thread for every completion the store applies, with the
 * slot key (a content digest, or rice:<crc>:<fmt>:<siz>), whether it left
 * pixels resident, and how long it was pending in pumps and nanoseconds (0
 * under the INLINE loader; ns only while a timing clock is installed). For
 * diagnostics; NULL uninstalls. */
typedef void (*MdkrModTextureInstallObserver)(const char *key, int resident,
                                              uint32_t latency_pumps,
                                              uint64_t latency_ns,
                                              size_t bytes);
void mdkr_mod_texture_set_install_observer(
    MdkrModTextureInstallObserver observer);

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

/* The same question for a Rice/GLideN64 pack, which names textures by an
 * identity the running game computes rather than by a content digest.
 *
 * `crc` comes from mdkr_rice_crc32() over the raw source texels; `fmt` and
 * `siz` are the RDP codes of the tile being uploaded and are part of the
 * identity, not decoration -- the same CRC under a different format is a
 * different texture.
 *
 * Returns 0 without touching the filesystem when no Rice pack is installed, so
 * a player with only digest-keyed packs never pays for this path. */
int  mdkr_mod_texture_lookup_rice(uint32_t crc, int fmt, int siz,
                                  MdkrModTexture *out);

/* How many Rice identities this run actually resolved to pixels. Zero with no
 * Rice pack installed, and zero if a pack is installed whose identities the
 * game never asks for -- which is the difference between "the pack loaded" and
 * "the pack is being used", and worth being able to state separately. */
int  mdkr_mod_texture_rice_resident(void);

/* Owner-thread, allocation-free snapshot. These are retained CPU payloads,
 * not process RSS or GPU memory; registry/ZIP allocations are not included. */
typedef struct MdkrModTextureMemoryStats {
    size_t pixel_bytes;     /* resident decoded pixels */
    size_t pixel_limit;     /* the whole budget, in-flight included */
    size_t table_bytes;
    size_t entries;
    /* Decoded pixels the worker holds that are not installed yet. Counted
     * against pixel_limit: with a worker running, residents are kept to
     * pixel_limit minus the in-flight cap (one eighth). In flight stays
     * within that cap except for one request alone, which may be a Rice
     * _rgb + _a pair of up to twice it (mod_texture_store.c gives the exact
     * bound). */
    size_t inflight_bytes;
    size_t pending;         /* textures queued or being decoded */
    /* Identities that became resident for the first time this store
     * generation, and their bytes. An identity counts once: a re-decode after
     * eviction does not add to it, but does add to reloads. */
    size_t first_resident;
    size_t first_resident_bytes;
    size_t reloads;
    size_t evictions;
} MdkrModTextureMemoryStats;
void mdkr_mod_texture_memory_stats(MdkrModTextureMemoryStats *out);

/* 1 when an ENABLED Rice pack is installed. The renderer tests this before
 * computing a Rice key, because that key is a full pass over the texture's
 * source bytes: without the test, every player with only digest-keyed packs
 * (or only a dump running) pays a second hash of every new texture for a
 * lookup that cannot succeed. */
int  mdkr_mod_texture_rice_active(void);

/* True when a lookup could possibly succeed: overrides on, a registry bound,
 * and at least one enabled pack in it. The renderer tests this before hashing
 * a texture, because computing a digest for a store that cannot answer is the
 * one cost this feature would otherwise impose on every install that has no
 * packs at all. */
bool mdkr_mod_texture_store_active(void);

/* ------------------------------------- resolve timing (issue #66) ---- */

/* What resolving pack textures has cost this process, as running totals.
 *
 * A replacement is decoded the first time the renderer binds it, inside the
 * display-list walk, so a scene change can pay for many large PNGs within one
 * frame. These totals say how much of a frame that was, split the way the cost
 * actually divides: finding the file, reading it (which, for a zipped pack,
 * includes inflating it) and decoding the PNG it holds.
 *
 * Totals, never reset -- not even by shutdown -- so any number of readers can
 * each keep their own last snapshot and difference against it (one per frame,
 * one per level generation) without taking the numbers from one another.
 *
 * All zero, and never a clock read, until a clock is installed: the store has
 * no clock of its own. The renderer installs one only when a diagnostic asked
 * for it (MDKR_TEXTURE_PACK_TIMING or the resource census), so an ordinary run
 * pays one null test per resolution and nothing else. */
typedef struct MdkrModTextureResolveTiming {
    uint64_t resolves;       /* slot resolutions run, absent answers included */
    uint64_t loaded;         /* resolutions that left decoded pixels resident */
    uint64_t probes;         /* pack lookups made (one pack open each) */
    uint64_t absent;         /* ...of which no enabled pack held the file */
    uint64_t files;          /* PNG files read and handed to the decoder */
    uint64_t file_bytes;     /* their size as read (after any ZIP inflate) */
    uint64_t decoded_bytes;  /* RGBA8 bytes the decoder produced from them */
    uint64_t open_ns;        /* locating the file: a name-index lookup on the
                              * owner thread (it used to be an archive open,
                              * which is what T12a measured here) */
    uint64_t absent_ns;      /* the part of open_ns spent on absent probes */
    uint64_t read_ns;        /* reading the entry's bytes (ZIP: inflate+CRC),
                              * on whichever thread decoded it */
    uint64_t decode_ns;      /* PNG header inspection + pixel decode, ditto */
    uint64_t resolve_ns;     /* lookups that resolved a slot, on the owner
                              * thread: the locate, queueing the request, and
                              * under the INLINE loader the read+decode too */
    /* Added for the off-frame loaders. Owner-thread time per frame is
     * resolve_ns + pump_ns; worker_ns is not on the game thread at all. */
    uint64_t pump_ns;        /* pump: draining and installing completions,
                              * evicting, and BUDGET-loader read+decode */
    uint64_t worker_ns;      /* whole requests executed on the worker */
    uint64_t queued;         /* requests created (PENDING entered) */
    uint64_t installed;      /* completions applied to their slot */
    uint64_t discarded;      /* completions dropped as stale or cancelled */
} MdkrModTextureResolveTiming;

/* NULL uninstalls, which is also the initial state. The clock is copied into
 * each request, so it must be callable from the worker thread too. */
void mdkr_mod_texture_timing_set_clock(MdkrModTextureClock clock);
/* The running totals. Always safe; all zero when no clock was ever set. */
void mdkr_mod_texture_timing_totals(MdkrModTextureResolveTiming *out);

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

/* Version of the `<digest>.txt` sidecar, written as its first line.
 *
 *   1 -- width, height, fmt, siz, first_seen. No version line: a record with
 *        no `dump_format` IS version 1, which is how corpora dumped before
 *        this field existed stay readable.
 *   2 -- adds source_width, source_height, source_line_bytes,
 *        source_size_bytes and source_texel_bytes, plus the
 *        `<digest>.texels` companion file holding the span itself.
 *
 * Version 1 is not a mode this code can still write; it is a shape a reader
 * must still accept. */
#define MDKR_MOD_TEXTURE_DUMP_FORMAT 2u

/* The raw N64 texel bytes behind one dumped texture, and how to address them.
 *
 * This exists for one caller that does not live in this repository: an offline
 * tool that has to recompute somebody else's hash -- a Rice pack's CRC-32, say
 * -- over the same bytes mdkr_mod_texture_digest() hashed. The decoded PNG
 * cannot serve that, because the decode is lossy in the direction that matters
 * (a 16-bit texel becomes eight-bit channels and its one coverage bit becomes
 * 0 or 255), so the source span has to be carried across separately or the
 * comparison cannot be made at all.
 *
 * `texels`/`texel_bytes` are the span EXACTLY as it was hashed, including the
 * caller's clamp against the end of the arena. `size_bytes` is what the tile
 * declared before that clamp, so a reader can tell a truncated span from a
 * whole one by comparing the two rather than having to guess.
 *
 * `width`/`height` are the SOURCE tile's logical geometry -- never the dumped
 * buffer's, which for an installed override is the replacement's own size and
 * has nothing to do with these bytes. `line_bytes` is the distance between
 * successive rows within the span, which is not `width` scaled by the size
 * code whenever the tile is padded. */
typedef struct MdkrModTextureSource {
    const uint8_t *texels;      /* the span the digest was taken over */
    size_t         texel_bytes; /* its length, after the arena clamp */
    uint32_t       line_bytes;  /* DkrTexCacheKey.source_line_bytes */
    uint32_t       size_bytes;  /* DkrTexCacheKey.source_size_bytes */
    int            width;       /* logical tile width, in texels */
    int            height;      /* logical tile height, in rows */
} MdkrModTextureSource;

/* Records the texture the renderer just resolved for `digest_hex`, for
 * tools/mod_texture_dump.py. `rgba` (tightly packed, `width * height * 4`
 * bytes) must be the pixels the renderer is about to upload for this bind --
 * an installed pack's override when one applied, the ROM decode otherwise --
 * so the dumped PNG is what the game actually displays, not a re-decode this
 * module invents on its own.
 *
 * `width` and `height` describe THAT BUFFER and nothing else. When an override
 * applied they are the replacement's own size, which is NOT the logical tile
 * size the caller normalises texcoords against -- those diverge by design
 * (gfx_pc_dkr.c, issue #34), and passing the wrong one of the two encodes a
 * correct buffer at a wrong stride: in bounds, silently sheared, and only
 * visible with a pack installed AND the dump on. `fmt` and `siz` stay the
 * SOURCE tile's, because they describe the picture the digest names rather
 * than the pixels written; they are the one pair here that is deliberately
 * not about `rgba`.
 *
 * `source` is the same divergence handled honestly rather than by a second
 * pair of int parameters nobody could keep straight: it is entirely about the
 * ROM bytes and never about `rgba`. NULL is legal and writes a record with no
 * source fields and no `.texels` -- a v2 record that a reader treats the way
 * it treats a v1 one, which is the only reason a null is worth accepting.
 *
 * Writes <dir>/<digest_hex>.png, <digest_hex>.txt (the key=value record
 * described at MDKR_MOD_TEXTURE_DUMP_FORMAT) and, when `source` carries a
 * span, <digest_hex>.texels (that span, raw) the first time this digest is
 * observed in the process; a no-op on every call after.
 *
 * Unconditionally a no-op unless MDKR_MOD_TEXTURE_DUMP is set: that is the
 * only cost this feature may impose when the variable is absent. Callers are
 * still expected to test mdkr_mod_texture_dump_active() before assembling the
 * arguments, because assembling them is not free even though the call is. */
void mdkr_mod_texture_dump_observe(const char *digest_hex, const uint8_t *rgba,
                                   int width, int height, uint8_t fmt,
                                   uint8_t siz,
                                   const MdkrModTextureSource *source,
                                   const char *first_seen);

#ifdef MDKR_MOD_TEXTURE_STORE_TESTING
/* Seams compiled only into tests/test_mod_texture_store.c's target. */
/* Make the next init's worker thread fail to start. */
void mdkr_mod_texture_store_test_fail_worker_start(int fail);
/* Replace the 512 MiB budget (0 restores it); applies at the next init. */
void mdkr_mod_texture_store_test_set_cache_limit(size_t bytes);
/* While held, the worker parks after taking a request and before reading it.
 * wait_parked() blocks until it is parked and returns 1, or returns 0 when no
 * worker is running. */
void mdkr_mod_texture_store_test_hold_worker(int hold);
int  mdkr_mod_texture_store_test_wait_parked(void);
/* Blocks until the worker has nothing queued and nothing in hand, WITHOUT
 * installing what it finished: completions stay queued for the next pump.
 * Returns 0 when no worker is running. */
int  mdkr_mod_texture_store_test_wait_idle(void);
/* The same, giving up after `timeout_ms`: 1 idle, 0 not (or no worker), so a
 * worker that never comes back fails a case instead of hanging it. */
int  mdkr_mod_texture_store_test_wait_idle_for(uint32_t timeout_ms);
/* Waits up to `timeout_ms` for the worker to be blocked on in-flight room
 * (its reservation does not fit until the owner drains completions). 1 when
 * it is, 0 on timeout or with no worker. */
int  mdkr_mod_texture_store_test_wait_room_blocked(uint32_t timeout_ms);
/* Tells the worker to stop and waits up to `timeout_ms` for its thread to
 * return. 1 when it has; shutdown still joins it and frees everything. 0 when
 * it is still running (a case then exits rather than join and hang). */
int  mdkr_mod_texture_store_test_stop_worker(uint32_t timeout_ms);
/* The most in-flight bytes held at once since the worker started. */
size_t mdkr_mod_texture_store_test_inflight_peak(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_TEXTURE_STORE_H */
