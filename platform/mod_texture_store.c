/**
 * mod_texture_store.c — see mod_texture_store.h.
 *
 * The store is one open-addressed table keyed by the 32-hex content digest
 * (or, for a Rice pack, a `rice:` identity). Every key the renderer ever asks
 * about gets a slot, including the ones no pack provides, because the
 * overwhelmingly common answer is "no" and working it out again every time a
 * texture missed the GPU cache would cost far more than the slot does. A slot
 * is therefore a decision that has already been made, not just a cache of
 * pixels:
 *
 *   SLOT_UNRESOLVED — the key is known, its answer is not yet (a fresh slot,
 *                     one whose pixels were evicted, or one whose pending
 *                     request a pack toggle retired).
 *   SLOT_ABSENT     — no enabled pack holds the file.
 *   SLOT_REJECTED   — a pack holds it, but it could not be used. Already
 *                     reported, once, and never retried.
 *   SLOT_PENDING    — a pack holds it and a request to read and decode it is
 *                     queued or running. Answers "not yet".
 *   SLOT_RESIDENT   — decoded RGBA8 held in `rgba`.
 *
 * ABSENT and REJECTED are final for the store generation. That is what makes
 * "report a bad PNG once" true by construction rather than by a separate flag
 * that could drift out of step with the state.
 *
 * Off the frame (issue #66). T12a measured a pack texture's first bind costing
 * an archive open per lookup (a full central-directory parse, ~1.5 ms, half of
 * them for a digest-named file a Rice pack can never hold), then the read and
 * the PNG decode, all inside the display-list walk: 170 ms frames on a scene
 * change. So:
 *
 *   - Locating a file is a lookup in the registry's name index, done here on
 *     the owner thread with no I/O. Packs are opened once per store
 *     generation, by init, and kept open.
 *   - Reading and decoding is a REQUEST, handed to the active loader: one
 *     background worker thread with its own open archives (native), or the
 *     owner thread inside mdkr_mod_texture_store_pump() under a time budget
 *     (the browser build, which has no threads, and the fallback when the
 *     thread will not start), or the lookup itself (INLINE, the 1.7 shape,
 *     kept for the author dump and for A/B measurement).
 *   - Results come back to the owner, which installs them once per frame
 *     under an install budget. Every request carries the load generation;
 *     a toggle, reload or shutdown moves it, and a completion from an older
 *     generation is discarded, its pixels freed, its slot never touched.
 *
 * Ownership. Every StoreSlot, the LRU clock, eviction, the counters, the wait
 * list and the load generation belong to the owner thread -- the one thread
 * the header's API may be called from. The worker sees exactly three things:
 * its own Executor (pack handles the owner opened for it before starting it
 * and closes after joining it), the TexRequest it popped (the spec half is
 * immutable once queued; it writes only the result half), and the buffers it
 * allocates for that result. The hand-off itself -- two queues, the stop flag,
 * the current generation and the in-flight byte count -- is LoaderShared,
 * every field of which is read and written only under its mutex.
 *
 * Eviction picks the least recently used RESIDENT slot by a linear scan. That
 * is O(table) per eviction, deliberately: eviction only happens once the cache
 * is holding hundreds of megabytes of pixels, and a scan of a few thousand
 * slots at that point is invisible next to the PNG decode that triggered it. A
 * PENDING slot has no pixels and is never a candidate: evicting it would
 * orphan its completion.
 */
#include "mod_texture_store.h"
#include "png_write_layout.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pack content is read through mod_source, never through a path this file
 * opens itself. That is what makes a zipped pack and an unpacked one the same
 * pack here: this module asks the registry which pack holds a name and reads
 * bytes back, with no branch on which kind answered. It also keeps the
 * Windows UTF-8/long-path boundary in exactly one place (fs_utf8, reached
 * through mod_source) rather than giving this file a second one.
 *
 * The author-dump path below still writes files, and that is the only reason
 * fs_utf8 is included here at all. */
#include "mod_source.h"
#if defined(_WIN32)
#include "fs_utf8.h"
#endif

/* The worker uses SDL's threads, as platform/a11y_speech_worker.c does. The
 * browser build is not compiled with threads, so it has no worker at all and
 * the BUDGET loader serves it; MDKR_MOD_TEXTURE_NO_WORKER lets an offline tool
 * build this file without SDL the same way. */
#if defined(__EMSCRIPTEN__) || defined(MDKR_MOD_TEXTURE_NO_WORKER)
#define STORE_HAS_WORKER 0
#else
#define STORE_HAS_WORKER 1
#include <SDL.h>
#endif

/* stb_image is instantiated in exactly one translation unit,
 * lib/stb/stb_image_impl.c, which is compiled with warnings off. This TU takes
 * the declarations only, and must configure the header identically or the two
 * would disagree about which entry points exist.
 *
 * The decoder runs on the worker thread. The pinned stb_image (v2.30) keeps
 * its failure reason thread-local, and nothing in the game sets its global
 * flip/premultiply switches, so a decode on the worker shares no state with
 * anything on the owner thread. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "stb_image_alloc.h"

/* stb_image_write, same arrangement: instantiated once, in the same TU as the
 * decoder, and taken here as declarations only. STBI_WRITE_NO_STDIO removes
 * the header's own fopen()-based entry points, which is what makes the *_to_func
 * callback the only way to reach the encoder from this file -- the same reason
 * STBI_NO_STDIO is set above, so the dump path writes through the port's own
 * UTF-8-safe fopen (mdkr_fopen_utf8 on Windows) rather than a second, narrower
 * one the header would otherwise carry. */
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* Total decoded pixels held at once, in-flight ones included. A 4K RGBA
 * texture is 64 MiB, so this is eight of them — generous for a pack that
 * replaces the HUD and a few tracks, and a hard stop for one that replaces
 * everything at 4K. */
#define MDKR_MOD_TEXTURE_CACHE_BYTES_MAX ((size_t)512u * 1024u * 1024u)

/* The share of that budget the worker may hold as decoded-but-not-installed
 * pixels: one eighth, 64 MiB, which is also the largest single texture the
 * admission rules below accept (4096 x 4096 x 4). While a worker runs,
 * residents are evicted down to the budget minus this, so resident plus
 * in-flight never exceeds the budget. */
#define MDKR_MOD_TEXTURE_INFLIGHT_SHARE 8u

/* Requests handed to the worker and not yet drained back. Bounds the shared
 * queue; anything past it waits on the owner's own list. */
#define MDKR_MOD_TEXTURE_DEPTH_MAX 64u

/* The largest side any shipping backend will upload. The store's only size
 * ceiling used to be the cache-bytes one above, which admits ~11585 per side,
 * while gfx_opengl_upload_texture() refuses anything over 4096. An oversized
 * replacement was therefore accepted, cached, and then refused by the renderer
 * every bind -- and a failed bind SKIPS the draw rather than falling back to
 * the ROM texture, so the object was invisible for the whole session while its
 * pixels held cache the store believed were in use. The importer already caps
 * here (tools/ricepack MAX_TEXTURE_DIMENSION); a pack assembled by any other
 * tool did not. Refusing at admission makes it a reported rejection instead. */
#define MDKR_MOD_TEXTURE_DIMENSION_MAX 4096

/* PNG decoding can use 16-bit RGBA intermediates even when we request 8-bit
 * output. Keep the admitted pixel budget inside the pinned decoder's integer
 * conversion bounds; increasing it requires a fresh decoder-boundary review. */
_Static_assert(MDKR_MOD_TEXTURE_CACHE_BYTES_MAX <= (size_t)INT_MAX / 2u,
               "PNG decode intermediates must fit signed integer sizes");

/* Largest PNG file this will read into memory before handing it to the decoder.
 * A legitimate 4K RGBA PNG compresses to a few tens of megabytes; anything past
 * this is either not a texture or is not one this store is willing to hold. */
#define MDKR_MOD_TEXTURE_FILE_BYTES_MAX ((size_t)64u * 1024u * 1024u)

/* Length of the digest the renderer addresses textures by. Fixed by
 * mdkr_mod_texture_digest(), which is a published contract. A slot key is a
 * digest or a `rice:<crc>:<fmt>:<siz>` identity, which is shorter. */
#define MDKR_MOD_TEXTURE_DIGEST_CHARS 32
#define MDKR_MOD_TEXTURE_KEY_CHARS    32

/* Distinct rejections written to the log before it stops listing them. Each
 * rejection is reported once, so this only matters for a pack that is broken
 * wholesale — and in that case the first few lines say everything the next
 * thousand would. */
#define MDKR_MOD_TEXTURE_REPORT_MAX 32
#define MDKR_MOD_TEXTURE_REASON_MAX 192

#define MDKR_MOD_TEXTURE_TABLE_MIN 256u

enum {
    SLOT_FREE = 0,
    SLOT_UNRESOLVED,
    SLOT_ABSENT,
    SLOT_REJECTED,
    SLOT_PENDING,
    SLOT_RESIDENT
};

typedef struct StoreSlot {
    char      digest[MDKR_MOD_TEXTURE_KEY_CHARS + 1];  /* the slot key */
    uint8_t  *rgba;
    int       width;
    int       height;
    size_t    bytes;
    uint64_t  last_use;
    /* The serial of the request a PENDING slot is waiting on. A completion
     * whose serial differs is not this slot's answer. */
    uint32_t  request;
    int       state;
    /* Resident at least once this store generation. Slots are never released,
     * so eviction and a later re-resolve cannot count an identity twice. */
    unsigned char counted;
} StoreSlot;

/* -------------------------------------------------------------- requests */

enum {
    RESULT_NONE = 0,
    RESULT_RESIDENT,
    RESULT_ABSENT,
    RESULT_REJECTED,
    /* Abandoned at a phase boundary: the generation moved or the store is
     * shutting down. Carries no pixels and no verdict about the file. */
    RESULT_CANCELLED
};

/* A pack that may serve one part of a request, and the name it stores that
 * file under: a digest-named texture matches whatever its case (see
 * mdkr_mod_registry_name_hits), so the spelling is the pack's own. */
typedef struct TexCandidate {
    int         pack;
    const char *path;
} TexCandidate;

/* One texture to read and decode. The owner fills the spec half, hands the
 * request over, and does not touch it again until it is handed back; the
 * executing thread fills the result half. Freed by the owner, always. */
typedef struct TexRequest {
    struct TexRequest *next;
    /* ---- spec: immutable once queued ---- */
    uint32_t    generation;
    uint32_t    serial;
    int         rice;
    char        key[MDKR_MOD_TEXTURE_KEY_CHARS + 1];
    /* Digest: part 0. Rice: part 0 = _all when `rice_all`, otherwise part 1
     * = _rgb and part 2 = _a. Each part lists, highest priority first, the
     * usable packs that may serve it with the name each stores it under --
     * the walk mdkr_mod_registry_open_file() would have made. An empty part
     * is a variant no usable pack supplies. */
    int           rice_all;
    TexCandidate *part[3];
    int           part_count[3];
    uint64_t    queued_pump;
    uint64_t    queued_ns;
    MdkrModTextureClock clock;   /* NULL: untimed */
    /* ---- result: written by the executing thread ---- */
    int         status;
    int         on_worker;
    uint8_t    *rgba;            /* stb-owned */
    int         width;
    int         height;
    size_t      bytes;
    size_t      reserved;        /* in-flight bytes this request holds */
    int         reasons;
    char        reason[2][MDKR_MOD_TEXTURE_REASON_MAX];
    uint64_t    files;
    uint64_t    file_bytes;
    uint64_t    decoded_bytes;
    uint64_t    read_ns;
    uint64_t    decode_ns;
    uint64_t    work_ns;
    /* Candidate arrays, then the path strings they point at, all owned with
     * the request so the executing thread needs nothing else. */
    TexCandidate candidates[];
} TexRequest;

typedef struct LoaderShared LoaderShared;

/* What a thread needs to execute requests: an open source per usable pack,
 * and, on the worker only, the hand-off it reserves in-flight bytes against.
 * Each loader has its own; no source is ever used by two threads. */
typedef struct Executor {
    MdkrModSource *sources[MDKR_MOD_MAX_PACKS];
    LoaderShared  *shared;
} Executor;

#if STORE_HAS_WORKER
/* The hand-off. Every field except `exec` is guarded by `lock`. `exec` is
 * the worker's own for as long as the thread runs: the owner fills it before
 * SDL_CreateThread and closes its sources after SDL_WaitThread. */
struct LoaderShared {
    SDL_mutex  *lock;
    SDL_cond   *wake_worker;    /* a request, a stop, room freed, a release */
    SDL_cond   *wake_owner;     /* a completion, a park, an idle worker */
    TexRequest *req_head;
    TexRequest *req_tail;
    TexRequest *done_head;
    TexRequest *done_tail;
    int         stop;
    int         busy;           /* the worker holds a request */
    uint32_t    generation;     /* the owner's current load generation */
    size_t      inflight;       /* bytes reserved by the worker or queued done */
    size_t      inflight_limit;
    int         hold;           /* test seam: park after taking a request */
    int         parked;
    Executor    exec;
};
#endif

/* ------------------------------------------------------------ owner state */

static const MdkrModRegistry *s_registry;
static int       s_enabled_packs;
/* Rice identities an enabled pack supplies, counted once at bind. The registry
 * has to walk its whole index to answer that -- 1663 entries for the pack this
 * was built against -- and the renderer asks on every texture upload, so the
 * answer is cached here the same way the enabled-pack count is. */
static int       s_rice_identities;
static bool      s_enabled = true;
static uint32_t  s_generation;

static StoreSlot *s_slots;
static size_t     s_slot_capacity;   /* always a power of two, or 0 */
static size_t     s_slot_count;      /* slots not in SLOT_FREE */
static size_t     s_resident_bytes;
/* Rice identities this run resolved to pixels: the difference between a pack
 * that loaded and a pack that is being used. */
static int         s_rice_resident;
static uint64_t   s_use_clock;
static int        s_reports;
static size_t     s_pending;
static size_t     s_first_resident;
static size_t     s_first_resident_bytes;
static size_t     s_reloads;
static size_t     s_evictions;

/* The loader. */
static MdkrModTextureLoader s_loader_request = MDKR_MOD_TEXTURE_LOADER_AUTO;
static MdkrModTextureLoader s_loader = MDKR_MOD_TEXTURE_LOADER_AUTO;
static uint32_t   s_load_generation;
static uint32_t   s_request_serial;
static uint32_t   s_settled_serial;
static uint64_t   s_pump_count;
/* Bit i: entries[i] is enabled, can supply textures, and the active loader
 * holds an open source for it. */
static uint64_t   s_usable_packs;
/* Some usable pack holds a digest-named texture (or could not be fully
 * indexed). When none does, a digest lookup is answered with no probe. */
static int        s_digest_capable;
static Executor   s_owner_exec;
/* Requests made but not yet handed to a loader thread: everything under the
 * BUDGET loader, and the overflow past the worker's queue depth. */
static TexRequest *s_wait_head;
static TexRequest *s_wait_tail;
static size_t     s_cache_limit = MDKR_MOD_TEXTURE_CACHE_BYTES_MAX;
static size_t     s_inflight_limit;
static MdkrModTextureInstallObserver s_observer;

#if STORE_HAS_WORKER
static LoaderShared *s_shared;
static SDL_Thread   *s_worker;
static uint32_t      s_outstanding;   /* handed to the worker, not drained */
#endif

/* Test seams; inert unless the test target sets them. */
static size_t     s_test_cache_limit;
#if STORE_HAS_WORKER
static int        s_test_fail_worker;
static int        s_test_hold;
#endif

/* Resolve timing (see the header). Installed by the renderer only when a
 * diagnostic asked; while it is NULL nothing below reads a clock or counts. */
static MdkrModTextureClock          s_timing_clock;
static MdkrModTextureResolveTiming  s_timing;

static const char *dump_directory(void);

/* ---------------------------------------------------------------- timing */

/* 0 when no clock is installed. Callers pass the value straight back to
 * timing_since() or timing_lap(), which check again, so an uninstrumented run
 * never calls a clock on either side of the work. */
static uint64_t timing_now(void) {
    return s_timing_clock != NULL ? s_timing_clock() : 0u;
}

static uint64_t timing_since(uint64_t start) {
    uint64_t now;
    if (s_timing_clock == NULL) return 0u;
    now = s_timing_clock();
    return now > start ? now - start : 0u;
}

/* Charges the time since `*mark` to `*bucket` and moves the mark to now, so
 * consecutive phases are timed back to back with one clock read per boundary. */
static void timing_lap(uint64_t *bucket, uint64_t *mark) {
    uint64_t now;
    if (s_timing_clock == NULL) return;
    now = s_timing_clock();
    if (now > *mark) *bucket += now - *mark;
    *mark = now;
}

/* The same, against a request's own copy of the clock: the executing thread
 * may not read s_timing_clock. */
static void request_lap(const TexRequest *req, uint64_t *bucket,
                        uint64_t *mark) {
    uint64_t now;
    if (req->clock == NULL) return;
    now = req->clock();
    if (now > *mark) *bucket += now - *mark;
    *mark = now;
}

/* ------------------------------------------------------------- reporting */

static void report_rejection(const char *digest, const char *reason) {
    if (s_reports >= MDKR_MOD_TEXTURE_REPORT_MAX) return;
    s_reports++;
    fprintf(stderr, "[MODS] texture %s: %s\n", digest,
            reason != NULL ? reason : "unusable");
    if (s_reports == MDKR_MOD_TEXTURE_REPORT_MAX) {
        fprintf(stderr,
                "[MODS] further unusable pack textures will not be listed\n");
    }
}

/* Executing thread: a rejection to report later, on the owner. */
static void request_reason(TexRequest *req, const char *reason) {
    if (req->reasons >= 2) return;
    snprintf(req->reason[req->reasons], sizeof req->reason[0], "%s",
             reason != NULL ? reason : "unusable");
    req->reasons++;
}

/* ----------------------------------------------------------------- table */

static uint64_t digest_hash(const char *digest) {
    /* FNV-1a. The digest is already well mixed, but its characters are hex, so
     * a naive prefix would only ever vary in four bits per byte. */
    uint64_t hash = 1469598103934665603ull;
    size_t   index;

    for (index = 0; digest[index] != '\0'; index++) {
        hash ^= (uint64_t)(unsigned char)digest[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

/* Places `digest` in `slots`, which must have a free slot available. Returns
 * the slot, already occupied by the key when it was not present. */
static StoreSlot *table_place(StoreSlot *slots, size_t capacity,
                              const char *digest, size_t *count) {
    size_t mask = capacity - 1u;
    size_t index = (size_t)digest_hash(digest) & mask;
    size_t probe;

    for (probe = 0; probe < capacity; probe++) {
        StoreSlot *slot = &slots[index];
        if (slot->state == SLOT_FREE) {
            memcpy(slot->digest, digest, strlen(digest) + 1);
            slot->state = SLOT_UNRESOLVED;
            if (count != NULL) (*count)++;
            return slot;
        }
        if (strcmp(slot->digest, digest) == 0) return slot;
        index = (index + 1u) & mask;
    }
    return NULL; /* unreachable while the load factor is enforced */
}

/* Grows to `capacity` slots. Returns 0 and leaves the store untouched when the
 * allocation fails, which costs cache hits but never correctness. */
static int table_grow(size_t capacity) {
    StoreSlot *slots;
    size_t     moved = 0;
    size_t     index;

    slots = (StoreSlot *)calloc(capacity, sizeof(*slots));
    if (slots == NULL) return 0;

    for (index = 0; index < s_slot_capacity; index++) {
        StoreSlot *old = &s_slots[index];
        StoreSlot *fresh;
        if (old->state == SLOT_FREE) continue;
        fresh = table_place(slots, capacity, old->digest, &moved);
        if (fresh == NULL) { /* cannot happen: capacity > s_slot_count */
            free(slots);
            return 0;
        }
        /* The whole slot, pending request and counted flag included: a field
         * left behind here is a field silently reset by growth. */
        *fresh = *old;
    }

    free(s_slots);
    s_slots = slots;
    s_slot_capacity = capacity;
    s_slot_count = moved;
    return 1;
}

/* The slot for `digest`, creating it as SLOT_UNRESOLVED when new. NULL only
 * when the table needed to grow and could not. */
static StoreSlot *slot_for(const char *digest) {
    if (s_slot_capacity == 0) {
        if (!table_grow(MDKR_MOD_TEXTURE_TABLE_MIN)) return NULL;
    } else if ((s_slot_count + 1u) * 10u >= s_slot_capacity * 7u) {
        /* Open addressing degrades sharply past ~70% occupancy, and a slot is
         * never released, so growth is the only thing keeping probes short. */
        if (!table_grow(s_slot_capacity * 2u) &&
            (s_slot_count + 1u) >= s_slot_capacity) {
            return NULL;
        }
    }
    return table_place(s_slots, s_slot_capacity, digest, &s_slot_count);
}

/* The existing slot for `key`, or NULL; never creates one. Completions find
 * their slot this way, by key, because growth moves slots. */
static StoreSlot *slot_find(const char *key) {
    size_t mask;
    size_t index;
    size_t probe;

    if (s_slot_capacity == 0) return NULL;
    mask = s_slot_capacity - 1u;
    index = (size_t)digest_hash(key) & mask;
    for (probe = 0; probe < s_slot_capacity; probe++) {
        StoreSlot *slot = &s_slots[index];
        if (slot->state == SLOT_FREE) return NULL;
        if (strcmp(slot->digest, key) == 0) return slot;
        index = (index + 1u) & mask;
    }
    return NULL;
}

/* -------------------------------------------------------------- eviction */

static void slot_release_pixels(StoreSlot *slot) {
    /* stbi_image_free, not free: the decoder's allocator is a compile-time
     * choice (STBI_MALLOC), and pairing its allocation with the wrong release
     * would only break on the day somebody sets it. */
    stbi_image_free(slot->rgba);
    slot->rgba = NULL;
    s_resident_bytes -= slot->bytes;
    slot->bytes = 0;
    slot->width = 0;
    slot->height = 0;
    slot->state = SLOT_UNRESOLVED;
}

/* What residents may hold: the whole budget, less the worker's in-flight
 * share while a worker can be holding pixels the owner has not installed. */
static size_t resident_cap(void) {
    return s_loader == MDKR_MOD_TEXTURE_LOADER_WORKER
               ? s_cache_limit - s_inflight_limit
               : s_cache_limit;
}

/* Drops least-recently-used residents until `incoming` more bytes fit under the
 * resident cap. Returns 0 when even an empty cache could not hold it. Only
 * SLOT_RESIDENT is ever a candidate. */
static int evict_for(size_t incoming) {
    const size_t cap = resident_cap();
    if (incoming > cap) return 0;

    while (s_resident_bytes + incoming > cap) {
        StoreSlot *victim = NULL;
        size_t     index;

        for (index = 0; index < s_slot_capacity; index++) {
            StoreSlot *candidate = &s_slots[index];
            if (candidate->state != SLOT_RESIDENT) continue;
            if (victim == NULL || candidate->last_use < victim->last_use) {
                victim = candidate;
            }
        }
        /* No resident left to drop, yet still over the cap: the accounting and
         * the cap disagree, which cannot happen, but returning beats looping. */
        if (victim == NULL) return 0;
        slot_release_pixels(victim);
        s_evictions++;
    }
    return 1;
}

/* ------------------------------------------------------------ file access */

#if defined(_WIN32)
static FILE *store_open_write(const char *path) {
    return mdkr_fopen_utf8(path, "wb");
}
#else
static FILE *store_open_write(const char *path) {
    return fopen(path, "wb");
}
#endif

/* The first candidate that can serve part `part`, highest priority first,
 * passing over one that cannot (absent, over the entry cap, unreadable) --
 * the same walk mdkr_mod_registry_open_file() makes, through sources that
 * are already open, each asked by its own spelling. Fills the entry's size.
 * Executing thread. */
static const TexCandidate *exec_locate(const Executor *ex,
                                       const TexRequest *req, int part,
                                       MdkrModSource **out_source,
                                       size_t *out_size) {
    int index;

    for (index = 0; index < req->part_count[part]; index++) {
        const TexCandidate *candidate = &req->part[part][index];
        MdkrModSource      *source;
        size_t              size = 0;
        int                 result;

        if (candidate->pack < 0 || candidate->pack >= MDKR_MOD_MAX_PACKS) {
            continue;
        }
        source = ex->sources[candidate->pack];
        if (source == NULL) continue;
        result = mdkr_mod_source_read(source, candidate->path, NULL, 0, &size);
        if (result == MDKR_MOD_SOURCE_BUFFER_TOO_SMALL ||
            result == MDKR_MOD_SOURCE_OK) {
            *out_source = source;
            *out_size = size;
            return candidate;
        }
    }
    return NULL;
}

/* Reads the whole of `relative` into a fresh buffer. Returns NULL on any
 * failure, with a player-readable reason — including a file past this store's
 * own ceiling, which is well under mod_source's, because a PNG that big is not
 * a texture whatever the container is willing to hand over.
 *
 * `size` is the answer mod_source already gave, which is safe to allocate
 * from only because mod_source bounded it: it refuses any entry over
 * MDKR_MOD_SOURCE_ENTRY_MAX before reporting a size at all, and the read below
 * refuses to write anything unless the whole entry fits. A declared length is
 * never allocated from on this path. */
static unsigned char *read_entry(MdkrModSource *source, const char *relative,
                                 size_t size, size_t *out_size,
                                 const char **out_reason) {
    unsigned char *buffer;
    int            result;

    *out_size = 0;
    *out_reason = NULL;
    if (size == 0) {
        *out_reason = "the file is empty";
        return NULL;
    }
    if (size > MDKR_MOD_TEXTURE_FILE_BYTES_MAX) {
        *out_reason = "the file is too large to load";
        return NULL;
    }

    buffer = (unsigned char *)malloc(size);
    if (buffer == NULL) {
        *out_reason = "there was not enough memory to load it";
        return NULL;
    }
    result = mdkr_mod_source_read(source, relative, buffer, size, &size);
    if (result != MDKR_MOD_SOURCE_OK) {
        free(buffer);
        *out_reason = mdkr_mod_source_result_text(result);
        return NULL;
    }
    *out_size = size;
    return buffer;
}

/* ------------------------------------------------- in-flight accounting */

#if STORE_HAS_WORKER
/* Worker: claims `bytes` of the in-flight share before the decoder allocates
 * them, waiting for the owner to drain completions when the share is full.
 * Never waits on its own request's reservations -- only other requests' can
 * be drained -- so one request may exceed the share when nothing else is in
 * flight. Returns 0, with the request marked cancelled, when the generation
 * moves or the store stops: the phase boundary at which work is abandoned. */
static int exec_reserve(Executor *ex, TexRequest *req, size_t bytes) {
    LoaderShared *shared = ex->shared;
    int           live;

    if (shared == NULL) return 1;
    SDL_LockMutex(shared->lock);
    while (!shared->stop && req->generation == shared->generation &&
           shared->inflight + bytes > shared->inflight_limit &&
           shared->inflight > req->reserved) {
        SDL_CondWait(shared->wake_worker, shared->lock);
    }
    live = !shared->stop && req->generation == shared->generation;
    if (live) {
        shared->inflight += bytes;
        req->reserved += bytes;
    }
    SDL_UnlockMutex(shared->lock);
    if (!live) req->status = RESULT_CANCELLED;
    return live;
}

static void exec_release(Executor *ex, TexRequest *req, size_t bytes) {
    LoaderShared *shared = ex->shared;
    if (shared == NULL || bytes == 0) return;
    SDL_LockMutex(shared->lock);
    shared->inflight -= bytes;
    req->reserved -= bytes;
    SDL_UnlockMutex(shared->lock);
}
#else
static int exec_reserve(Executor *ex, TexRequest *req, size_t bytes) {
    (void)ex; (void)req; (void)bytes;
    return 1;
}

static void exec_release(Executor *ex, TexRequest *req, size_t bytes) {
    (void)ex; (void)req; (void)bytes;
}
#endif

/* ------------------------------------------------------------- resolution */

/* Load one pack PNG into RGBA. Executing thread.
 *
 * Every admission rule a replacement texture has to pass lives here and only
 * here: the cap on the file, the header inspection that establishes the decode
 * budget BEFORE the pixels are allocated, the dimension ceiling, and the
 * requirement that the decoded size match the admitted header. Rice textures go
 * through the identical checks -- a pack from a stranger is a pack from a
 * stranger whichever convention names its files -- and so does every loader:
 * the worker runs exactly this code.
 *
 * Returns stbi-owned pixels whose size is held as an in-flight reservation on
 * the request, or NULL. `out_absent`, when given, separates the ordinary "no
 * candidate pack holds this path" from a rejection whose reason has been
 * recorded on the request. A cancelled request returns NULL with
 * req->status == RESULT_CANCELLED. */
static unsigned char *exec_load_png(Executor *ex, TexRequest *req, int part,
                                    int *out_width, int *out_height,
                                    int *out_absent) {
    const TexCandidate *found;
    MdkrModSource *source = NULL;
    const char    *reason = NULL;
    unsigned char *file_bytes;
    size_t         size = 0;
    size_t         file_size = 0;
    size_t         declared_bytes;
    unsigned char *pixels;
    int            declared_width = 0;
    int            declared_height = 0;
    int            declared_channels = 0;
    int            width = 0;
    int            height = 0;
    int            channels = 0;
    /* Phase boundaries: locating and reading the entry, then decoding it. */
    uint64_t       phase_mark = req->clock != NULL ? req->clock() : 0u;

    if (out_absent != NULL) *out_absent = 0;
    found = exec_locate(ex, req, part, &source, &size);
    if (found == NULL) {
        if (out_absent != NULL) *out_absent = 1;
        request_lap(req, &req->read_ns, &phase_mark);
        return NULL;
    }
    file_bytes = read_entry(source, found->path, size, &file_size, &reason);
    request_lap(req, &req->read_ns, &phase_mark);
    if (file_bytes == NULL) {
        request_reason(req, reason);
        return NULL;
    }
    req->files++;
    req->file_bytes += file_size;
    /* The cap above is far below INT_MAX, so this only documents the bound the
     * decoder's int-typed length depends on. */
    if (file_size > (size_t)INT_MAX) {
        free(file_bytes);
        request_reason(req, "the file is too large to decode");
        return NULL;
    }

    /* The header before the pixels. stbi_info_from_memory() parses the PNG
     * header and stops, so the size the file CLAIMS costs a few hundred bytes
     * of parsing to learn, while decoding to find out costs the whole picture.
     * Without this the cap below bounds only what is RETAINED: a pack PNG of a
     * few hundred kilobytes that declares 32768x32768 is a gigabyte the
     * allocator has already handed over by the time anything asks whether it
     * fits. Packs are files a player downloads from strangers, so "we free it
     * again immediately" is not an answer.
     *
     * Measured in 64 bits and against the whole budget, the same cap
     * evict_for() can never exceed. The declared component count is
     * deliberately ignored: this store always asks the decoder for RGBA, so
     * four bytes a pixel is what a decode of this file would cost whatever the
     * file itself stores -- and a one-channel image is precisely the case
     * where the two differ by four.
     *
     * Header inspection is admission, not an optional optimization: without
     * dimensions we cannot establish the decode budget. stbi_info() already
     * supplies stb's own failure reason (thread-local in the pinned version),
     * so reporting it does not require a second parse through the pixel
     * decoder. Retain the post-decode checks and require the decoded
     * dimensions to match the admitted header. */
    if (stbi_info_from_memory(file_bytes, (int)file_size, &declared_width,
                              &declared_height, &declared_channels) == 0) {
        free(file_bytes);
        request_reason(req, stbi_failure_reason());
        return NULL;
    }
    if (declared_width <= 0 || declared_height <= 0) {
        free(file_bytes);
        request_reason(req, "the image has no pixels");
        return NULL;
    }
    if (declared_width > MDKR_MOD_TEXTURE_DIMENSION_MAX ||
        declared_height > MDKR_MOD_TEXTURE_DIMENSION_MAX) {
        /* Sized for the worst case the compiler can prove, not the typical
         * one: three %d are up to 11 characters each, and mingw's gcc refuses
         * the truncation this literal would suffer at 128. A rejection reason
         * cut off mid-sentence is exactly the log line nobody can act on. */
        char too_wide[MDKR_MOD_TEXTURE_REASON_MAX];
        snprintf(too_wide, sizeof too_wide,
                 "the image declares %dx%d; no backend uploads a side over %d, "
                 "so this would silently skip every draw that binds it",
                 declared_width, declared_height,
                 MDKR_MOD_TEXTURE_DIMENSION_MAX);
        free(file_bytes);
        request_reason(req, too_wide);
        return NULL;
    }
    {
        uint64_t declared = (uint64_t)declared_width *
                            (uint64_t)declared_height * 4u;
        if (declared > (uint64_t)MDKR_MOD_TEXTURE_CACHE_BYTES_MAX) {
            char too_big[MDKR_MOD_TEXTURE_REASON_MAX];
            snprintf(too_big, sizeof too_big,
                     "the image declares %dx%d, larger than the whole texture "
                     "cache", declared_width, declared_height);
            free(file_bytes);
            request_reason(req, too_big);
            return NULL;
        }
        declared_bytes = (size_t)declared;
    }

    /* Admitted. Claim its decoded size before the decoder allocates it; this
     * is also where a request the owner has since retired is abandoned. */
    if (!exec_reserve(ex, req, declared_bytes)) {
        free(file_bytes);
        return NULL;
    }
    /* The header bounds the decode as well as the result: the zlib stage
     * otherwise grows its buffer until the IDAT stream ends, so a few
     * megabytes of crafted data could claim gigabytes on the way to a
     * picture the header already admitted. The bound is per thread, set
     * around this one decode. */
    {
        const size_t decode_bound = mdkr_stbi_png_allocation_bound(
            declared_width, declared_height, declared_channels,
            stbi_is_16_bit_from_memory(file_bytes, (int)file_size), 4,
            file_size);
        const size_t previous_bound =
            mdkr_stbi_set_allocation_limit(decode_bound);
        pixels = decode_bound != 0u
                     ? stbi_load_from_memory(file_bytes, (int)file_size,
                                             &width, &height, &channels, 4)
                     : NULL;
        (void)mdkr_stbi_set_allocation_limit(previous_bound);
    }
    free(file_bytes);
    /* Header inspection plus the decode. A file refused from its header alone
     * above cost microseconds and is left to the request's whole time. */
    request_lap(req, &req->decode_ns, &phase_mark);
    if (pixels != NULL && width > 0 && height > 0) {
        req->decoded_bytes += (uint64_t)width * (uint64_t)height * 4u;
    }
    if (pixels == NULL) {
        /* stb's own wording, so the log names the actual defect in the PNG
         * rather than this module's guess at it. */
        exec_release(ex, req, declared_bytes);
        request_reason(req, stbi_failure_reason());
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        exec_release(ex, req, declared_bytes);
        request_reason(req, "the image has no pixels");
        return NULL;
    }

    if (width != declared_width || height != declared_height) {
        stbi_image_free(pixels);
        exec_release(ex, req, declared_bytes);
        request_reason(req,
                       "the image changed dimensions during bounded decode");
        return NULL;
    }

    *out_width = width;
    *out_height = height;
    return pixels;
}

/* Executes one request on `ex`, filling its result. Runs on the worker for the
 * WORKER loader and on the owner thread otherwise; it touches nothing but `ex`
 * and `req`.
 *
 * The Rice variants are the format's own. `_all` is a complete image and wins
 * when a pack supplies it. Otherwise `_rgb` carries colour and `_a` carries
 * alpha in its RED channel -- an author exports the two halves from tools
 * that cannot write a single RGBA file. An `_rgb` with no partner is OPAQUE,
 * deliberately: treating a missing alpha half as transparent turns every
 * unpaired texture invisible, which is the failure this project already
 * recorded when three orphan halves were refused during the offline import. */
static void request_execute(Executor *ex, TexRequest *req) {
    const uint64_t start = req->clock != NULL ? req->clock() : 0u;
    unsigned char *pixels = NULL;
    int            width = 0;
    int            height = 0;

    req->status = RESULT_NONE;
    if (!req->rice) {
        int absent = 0;
        pixels = exec_load_png(ex, req, 0, &width, &height, &absent);
        if (pixels == NULL && req->status != RESULT_CANCELLED) {
            /* Absent is ordinary (the file left a folder pack after the
             * scan); anything else already recorded its reason. */
            req->status = absent ? RESULT_ABSENT : RESULT_REJECTED;
        }
    } else {
        if (req->rice_all) {
            pixels = exec_load_png(ex, req, 0, &width, &height, NULL);
        } else {
            pixels = exec_load_png(ex, req, 1, &width, &height, NULL);
            if (pixels != NULL) {
                const size_t   count = (size_t)width * (size_t)height;
                const size_t   own = count * 4u;
                int            alpha_width = 0;
                int            alpha_height = 0;
                unsigned char *alpha = req->part_count[2] == 0 ? NULL :
                    exec_load_png(ex, req, 2, &alpha_width, &alpha_height,
                                  NULL);
                size_t         i;
                if (req->status == RESULT_CANCELLED) {
                    stbi_image_free(pixels);
                    exec_release(ex, req, own);
                    pixels = NULL;
                } else if (alpha != NULL && alpha_width == width &&
                           alpha_height == height) {
                    for (i = 0; i < count; ++i) {
                        pixels[i * 4u + 3u] = alpha[i * 4u];  /* RED is alpha */
                    }
                } else {
                    /* No partner, or one that does not line up pixel for
                     * pixel. Opaque is the honest reading; a mismatched half
                     * is not alpha for THIS image and guessing would be
                     * worse. */
                    for (i = 0; i < count; ++i) pixels[i * 4u + 3u] = 255u;
                    if (alpha != NULL) {
                        request_reason(req,
                                       "its _a half is a different size from "
                                       "its _rgb half; the texture is opaque");
                    }
                }
                if (alpha != NULL) {
                    stbi_image_free(alpha);
                    exec_release(ex, req,
                                 (size_t)alpha_width * (size_t)alpha_height * 4u);
                }
            }
        }
        if (pixels == NULL && req->status != RESULT_CANCELLED) {
            /* A file that vanished between indexing and use reports nothing
             * from the loader. It used to fail in silence -- the pack said it
             * supplied the texture, nothing appeared, and the log was empty. */
            request_reason(req, "the pack file it names could not be opened");
            req->status = RESULT_REJECTED;
        }
    }
    if (pixels != NULL) {
        req->status = RESULT_RESIDENT;
        req->rgba = (uint8_t *)pixels;
        req->width = width;
        req->height = height;
        req->bytes = (size_t)width * (size_t)height * 4u;
    }
    if (req->clock != NULL) {
        const uint64_t now = req->clock();
        req->work_ns = now > start ? now - start : 0u;
    }
}

/* --------------------------------------------------------- request lifecycle */

static void request_free(TexRequest *req) {
    if (req == NULL) return;
    stbi_image_free(req->rgba);
    free(req);
}

/* A request for `key`, owner thread. Candidates and their path strings are
 * copied in, so the request owns everything the executing thread will read.
 * NULL when out of memory, which leaves the slot unresolved and the next
 * lookup to try again. */
static TexRequest *request_new(const char *key, int rice, int rice_all,
                               const TexCandidate *const part[3],
                               const int count[3]) {
    size_t      candidates = 0;
    size_t      strings = 0;
    TexRequest *req;
    char       *cursor;
    size_t      next = 0;
    int         i;
    int         j;

    for (i = 0; i < 3; i++) {
        candidates += (size_t)count[i];
        for (j = 0; j < count[i]; j++) strings += strlen(part[i][j].path) + 1u;
    }
    req = (TexRequest *)calloc(1, sizeof *req +
                                  candidates * sizeof(TexCandidate) + strings);
    if (req == NULL) return NULL;
    cursor = (char *)(req->candidates + candidates);
    for (i = 0; i < 3; i++) {
        req->part[i] = req->candidates + next;
        req->part_count[i] = count[i];
        for (j = 0; j < count[i]; j++) {
            const size_t length = strlen(part[i][j].path) + 1u;
            memcpy(cursor, part[i][j].path, length);
            req->candidates[next].pack = part[i][j].pack;
            req->candidates[next].path = cursor;
            cursor += length;
            next++;
        }
    }
    req->generation = s_load_generation;
    req->serial = ++s_request_serial;
    if (req->serial == 0) req->serial = ++s_request_serial;
    req->rice = rice;
    req->rice_all = rice_all;
    snprintf(req->key, sizeof req->key, "%s", key);
    req->queued_pump = s_pump_count;
    req->clock = s_timing_clock;
    req->queued_ns = req->clock != NULL ? req->clock() : 0u;
    return req;
}

/* Adds what a finished request cost to the running totals. Owner thread. */
static void request_account(const TexRequest *req) {
    if (s_timing_clock == NULL) return;
    s_timing.files += req->files;
    s_timing.file_bytes += req->file_bytes;
    s_timing.decoded_bytes += req->decoded_bytes;
    s_timing.read_ns += req->read_ns;
    s_timing.decode_ns += req->decode_ns;
    if (req->on_worker) s_timing.worker_ns += req->work_ns;
}

/* Applies a fresh result to its slot. Owner thread. Reports every rejection
 * the executing thread recorded, in order, against the slot's own key. */
static void request_apply(StoreSlot *slot, TexRequest *req) {
    int index;

    for (index = 0; index < req->reasons; index++) {
        report_rejection(slot->digest, req->reason[index]);
    }
    switch (req->status) {
    case RESULT_RESIDENT:
        if (!evict_for(req->bytes)) {
            stbi_image_free(req->rgba);
            req->rgba = NULL;
            slot->state = SLOT_REJECTED;
            report_rejection(slot->digest,
                             "the image is larger than the whole texture cache");
            break;
        }
        slot->rgba = req->rgba;
        req->rgba = NULL;
        slot->width = req->width;
        slot->height = req->height;
        slot->bytes = req->bytes;
        slot->state = SLOT_RESIDENT;
        /* Fresh, not least-recently-used: the next install in this same pump
         * must not evict the texture the renderer is about to draw. */
        slot->last_use = ++s_use_clock;
        s_resident_bytes += req->bytes;
        /* Counted once per identity, not once per resolve: under cache
         * pressure a texture is evicted and re-resolved, and reporting that as
         * more textures than the pack supplies would overstate the evidence. */
        if (!slot->counted) {
            slot->counted = 1;
            s_first_resident++;
            s_first_resident_bytes += req->bytes;
            if (req->rice) s_rice_resident++;
        } else {
            s_reloads++;
        }
        if (s_timing_clock != NULL) s_timing.loaded++;
        break;
    case RESULT_ABSENT:
        slot->state = SLOT_ABSENT;
        break;
    default:
        slot->state = SLOT_REJECTED;
        break;
    }
    if (s_timing_clock != NULL) s_timing.installed++;
}

static void request_observe(const StoreSlot *slot, const TexRequest *req) {
    uint64_t latency_ns = 0;
    if (s_observer == NULL) return;
    if (req->clock != NULL && s_timing_clock != NULL) {
        const uint64_t now = s_timing_clock();
        latency_ns = now > req->queued_ns ? now - req->queued_ns : 0u;
    }
    s_observer(slot->digest, slot->state == SLOT_RESIDENT,
               (uint32_t)(s_pump_count - req->queued_pump), latency_ns,
               slot->state == SLOT_RESIDENT ? slot->bytes : 0u);
}

/* A request handed back by a loader, owner thread: install it if it is still
 * the answer its slot is waiting for, otherwise discard it. Frees it. */
static void request_complete(TexRequest *req) {
    StoreSlot *slot = slot_find(req->key);
    const int  fresh = req->status != RESULT_CANCELLED &&
                       req->status != RESULT_NONE &&
                       req->generation == s_load_generation && slot != NULL &&
                       slot->state == SLOT_PENDING &&
                       slot->request == req->serial;

    request_account(req);
    if (!fresh) {
        if (s_timing_clock != NULL) s_timing.discarded++;
        request_free(req);
        return;
    }
    s_pending--;
    request_apply(slot, req);
    s_settled_serial++;
    request_observe(slot, req);
    request_free(req);
}

/* ------------------------------------------------------------------ loaders */

static void wait_push(TexRequest *req) {
    req->next = NULL;
    if (s_wait_tail != NULL) s_wait_tail->next = req;
    else s_wait_head = req;
    s_wait_tail = req;
}

static TexRequest *wait_pop(void) {
    TexRequest *req = s_wait_head;
    if (req == NULL) return NULL;
    s_wait_head = req->next;
    if (s_wait_head == NULL) s_wait_tail = NULL;
    req->next = NULL;
    return req;
}

static void wait_discard_all(void) {
    TexRequest *req;
    while ((req = wait_pop()) != NULL) {
        if (s_timing_clock != NULL) s_timing.discarded++;
        request_free(req);
    }
}

static void executor_close(Executor *ex) {
    int index;
    for (index = 0; index < MDKR_MOD_MAX_PACKS; index++) {
        mdkr_mod_source_close(ex->sources[index]);
        ex->sources[index] = NULL;
    }
    ex->shared = NULL;
}

/* Opens one source per pack in `packs`. Returns the packs that opened. */
static uint64_t executor_open(Executor *ex, uint64_t packs) {
    uint64_t opened = 0;
    int      index;

    memset(ex, 0, sizeof *ex);
    for (index = 0; index < s_registry->count && index < MDKR_MOD_MAX_PACKS;
         index++) {
        const MdkrModEntry *entry = &s_registry->entries[index];
        if ((packs & (UINT64_C(1) << index)) == 0) continue;
        ex->sources[index] = mdkr_mod_source_open(entry->root, entry->is_zip);
        if (ex->sources[index] != NULL) opened |= UINT64_C(1) << index;
    }
    return opened;
}

/* BUDGET loader: one queued request, executed and completed on the owner. */
static size_t owner_run_one(void) {
    TexRequest *req = wait_pop();
    size_t      bytes;
    if (req == NULL) return 0;
    request_execute(&s_owner_exec, req);
    bytes = req->status == RESULT_RESIDENT ? req->bytes : 0u;
    request_complete(req);
    return bytes;
}

#if STORE_HAS_WORKER
static void shared_push(TexRequest **head, TexRequest **tail, TexRequest *req) {
    req->next = NULL;
    if (*tail != NULL) (*tail)->next = req;
    else *head = req;
    *tail = req;
}

static int SDLCALL worker_main(void *opaque) {
    LoaderShared *shared = (LoaderShared *)opaque;

    SDL_LockMutex(shared->lock);
    for (;;) {
        TexRequest *req;
        int         stale;

        while (!shared->stop && shared->req_head == NULL) {
            SDL_CondWait(shared->wake_worker, shared->lock);
        }
        if (shared->stop) break;
        req = shared->req_head;
        shared->req_head = req->next;
        if (shared->req_head == NULL) shared->req_tail = NULL;
        req->next = NULL;
        shared->busy = 1;
        /* Test seam: park holding the request, before any I/O. Inert unless a
         * test set `hold`; a stop always releases it. */
        while (shared->hold && !shared->stop) {
            shared->parked = 1;
            SDL_CondBroadcast(shared->wake_owner);
            SDL_CondWait(shared->wake_worker, shared->lock);
        }
        shared->parked = 0;
        stale = shared->stop || req->generation != shared->generation;
        SDL_UnlockMutex(shared->lock);

        req->on_worker = 1;
        if (stale) req->status = RESULT_CANCELLED;
        else request_execute(&shared->exec, req);

        SDL_LockMutex(shared->lock);
        shared_push(&shared->done_head, &shared->done_tail, req);
        shared->busy = 0;
        SDL_CondBroadcast(shared->wake_owner);
    }
    SDL_UnlockMutex(shared->lock);
    return 0;
}

/* Hands waiting requests to the worker while its queue has room. */
static void loader_submit(void) {
    LoaderShared *shared = s_shared;
    int           handed = 0;

    if (shared == NULL || s_wait_head == NULL ||
        s_outstanding >= MDKR_MOD_TEXTURE_DEPTH_MAX) {
        return;
    }
    SDL_LockMutex(shared->lock);
    while (s_wait_head != NULL && s_outstanding < MDKR_MOD_TEXTURE_DEPTH_MAX) {
        shared_push(&shared->req_head, &shared->req_tail, wait_pop());
        s_outstanding++;
        handed = 1;
    }
    if (handed) SDL_CondBroadcast(shared->wake_worker);
    SDL_UnlockMutex(shared->lock);
}

/* Takes finished requests back from the worker -- up to `max_count` installs
 * and `max_bytes` of pixels, at least one, or everything when `all` -- and
 * completes them on the owner. Their in-flight bytes are released as they are
 * taken: from here on they are residents or freed. */
static void loader_drain(uint32_t max_count, size_t max_bytes, int all) {
    LoaderShared *shared = s_shared;
    TexRequest   *head = NULL;
    TexRequest   *tail = NULL;
    uint32_t      count = 0;
    uint32_t      taken = 0;
    size_t        bytes = 0;

    if (shared == NULL) return;
    SDL_LockMutex(shared->lock);
    while (shared->done_head != NULL) {
        TexRequest  *req = shared->done_head;
        const size_t size = req->status == RESULT_RESIDENT ? req->bytes : 0u;
        if (!all && count > 0 &&
            (count >= max_count || bytes + size > max_bytes)) {
            break;
        }
        shared->done_head = req->next;
        if (shared->done_head == NULL) shared->done_tail = NULL;
        shared->inflight -= req->reserved;
        req->reserved = 0;
        req->next = NULL;
        if (tail != NULL) tail->next = req;
        else head = req;
        tail = req;
        taken++;
        /* A cancelled request costs the frame nothing to discard. */
        if (req->status != RESULT_CANCELLED) {
            count++;
            bytes += size;
        }
    }
    if (taken > 0) SDL_CondBroadcast(shared->wake_worker);
    SDL_UnlockMutex(shared->lock);

    s_outstanding -= taken;
    while (head != NULL) {
        TexRequest *req = head;
        head = req->next;
        req->next = NULL;
        request_complete(req);
    }
}

static void shared_free_list(TexRequest *req) {
    while (req != NULL) {
        TexRequest *next = req->next;
        request_free(req);
        req = next;
    }
}

/* Starts the worker over `packs`. 0, with nothing left behind, when the
 * primitives or the thread cannot be created. */
static int worker_start(uint64_t packs, uint64_t *out_opened) {
    LoaderShared *shared = (LoaderShared *)calloc(1, sizeof *shared);
    uint64_t      opened;

    *out_opened = 0;
    if (shared == NULL) return 0;
    opened = executor_open(&shared->exec, packs);
    shared->exec.shared = shared;
    shared->generation = s_load_generation;
    shared->inflight_limit = s_inflight_limit;
    shared->hold = s_test_hold;
    shared->lock = SDL_CreateMutex();
    shared->wake_worker = SDL_CreateCond();
    shared->wake_owner = SDL_CreateCond();
    if (shared->lock != NULL && shared->wake_worker != NULL &&
        shared->wake_owner != NULL && !s_test_fail_worker) {
        s_worker = SDL_CreateThread(worker_main, "mdkr-texture-pack", shared);
    }
    if (s_worker == NULL) {
        if (shared->wake_owner != NULL) SDL_DestroyCond(shared->wake_owner);
        if (shared->wake_worker != NULL) SDL_DestroyCond(shared->wake_worker);
        if (shared->lock != NULL) SDL_DestroyMutex(shared->lock);
        executor_close(&shared->exec);
        free(shared);
        return 0;
    }
    s_shared = shared;
    s_outstanding = 0;
    *out_opened = opened;
    return 1;
}

/* Stops and joins the worker, then frees every request it had not handed
 * back and closes its sources. Safe with no worker. */
static void worker_stop(void) {
    LoaderShared *shared = s_shared;
    if (shared == NULL) return;
    SDL_LockMutex(shared->lock);
    shared->stop = 1;
    SDL_CondBroadcast(shared->wake_worker);
    SDL_UnlockMutex(shared->lock);
    SDL_WaitThread(s_worker, NULL);
    s_worker = NULL;
    s_shared = NULL;
    /* Joined: nothing else can touch these now. */
    shared_free_list(shared->req_head);
    shared_free_list(shared->done_head);
    executor_close(&shared->exec);
    SDL_DestroyCond(shared->wake_owner);
    SDL_DestroyCond(shared->wake_worker);
    SDL_DestroyMutex(shared->lock);
    free(shared);
    s_outstanding = 0;
}
#endif /* STORE_HAS_WORKER */

/* The loader the next init should run. */
static MdkrModTextureLoader loader_choose(void) {
    MdkrModTextureLoader loader = s_loader_request;
    if (loader == MDKR_MOD_TEXTURE_LOADER_AUTO) {
        const char *env = getenv("MDKR_TEXTURE_PACK_LOADER");
        if (env != NULL && strcmp(env, "worker") == 0) {
            loader = MDKR_MOD_TEXTURE_LOADER_WORKER;
        } else if (env != NULL && strcmp(env, "budget") == 0) {
            loader = MDKR_MOD_TEXTURE_LOADER_BUDGET;
        } else if (env != NULL && strcmp(env, "inline") == 0) {
            loader = MDKR_MOD_TEXTURE_LOADER_INLINE;
        } else if (dump_directory() != NULL) {
            /* The author dump records what a texture is FIRST drawn with,
             * and a pending replacement is first drawn as the original. */
            loader = MDKR_MOD_TEXTURE_LOADER_INLINE;
        } else {
            loader = MDKR_MOD_TEXTURE_LOADER_WORKER;
        }
    }
#if !STORE_HAS_WORKER
    if (loader == MDKR_MOD_TEXTURE_LOADER_WORKER) {
        loader = MDKR_MOD_TEXTURE_LOADER_BUDGET;
    }
#endif
    return loader;
}

/* Opens the enabled texture packs for the chosen loader. A pack that holds
 * no texture a lookup could find -- music only, say -- is not opened at all,
 * and with no such pack there is no loader and no thread. */
static void loader_start(void) {
    MdkrModTextureLoader loader = loader_choose();
    uint64_t             wanted = 0;
    uint64_t             opened = 0;
    int                  index;

    for (index = 0; index < s_registry->count && index < MDKR_MOD_MAX_PACKS;
         index++) {
        const MdkrModEntry *entry = &s_registry->entries[index];
        if (!entry->manifest.enabled) continue;
        if (entry->digest_textures > 0 || !entry->names_indexed ||
            mdkr_mod_registry_pack_rice_count(s_registry, index) > 0) {
            wanted |= UINT64_C(1) << index;
        }
    }
    if (wanted == 0) return;

#if STORE_HAS_WORKER
    if (loader == MDKR_MOD_TEXTURE_LOADER_WORKER) {
        if (worker_start(wanted, &opened)) {
            s_loader = MDKR_MOD_TEXTURE_LOADER_WORKER;
        } else {
            fprintf(stderr, "[MODS] the texture loading thread could not be "
                            "started; pack textures load between frames "
                            "instead\n");
            loader = MDKR_MOD_TEXTURE_LOADER_BUDGET;
        }
    }
#endif
    if (s_loader != MDKR_MOD_TEXTURE_LOADER_WORKER) {
        opened = executor_open(&s_owner_exec, wanted);
        s_loader = loader;
    }
    s_usable_packs = opened;
    for (index = 0; index < s_registry->count && index < MDKR_MOD_MAX_PACKS;
         index++) {
        const MdkrModEntry *entry = &s_registry->entries[index];
        if ((opened & (UINT64_C(1) << index)) == 0) continue;
        if (entry->digest_textures > 0 || !entry->names_indexed) {
            s_digest_capable = 1;
        }
    }
}

static void loader_stop(void) {
#if STORE_HAS_WORKER
    worker_stop();
#endif
    wait_discard_all();
    executor_close(&s_owner_exec);
    s_loader = MDKR_MOD_TEXTURE_LOADER_AUTO;
    s_usable_packs = 0;
    s_digest_capable = 0;
}

/* A new load generation: every queued request is now stale and every pending
 * slot goes back to unresolved, to be asked for again. Owner thread. */
static void loader_retarget(void) {
    size_t index;

    s_load_generation++;
    wait_discard_all();
#if STORE_HAS_WORKER
    if (s_shared != NULL) {
        SDL_LockMutex(s_shared->lock);
        s_shared->generation = s_load_generation;
        SDL_CondBroadcast(s_shared->wake_worker);
        SDL_UnlockMutex(s_shared->lock);
    }
#endif
    for (index = 0; index < s_slot_capacity; index++) {
        if (s_slots[index].state == SLOT_PENDING) {
            s_slots[index].state = SLOT_UNRESOLVED;
            s_slots[index].request = 0;
        }
    }
    s_pending = 0;
    s_settled_serial++;
}

/* Hands a new request to the active loader, or, under INLINE, runs it now. */
static void loader_enqueue(StoreSlot *slot, TexRequest *req) {
    if (s_loader == MDKR_MOD_TEXTURE_LOADER_INLINE) {
        request_execute(&s_owner_exec, req);
        request_account(req);
        request_apply(slot, req);
        s_settled_serial++;
        request_observe(slot, req);
        request_free(req);
        return;
    }
    slot->state = SLOT_PENDING;
    slot->request = req->serial;
    s_pending++;
    if (s_timing_clock != NULL) s_timing.queued++;
    wait_push(req);
#if STORE_HAS_WORKER
    if (s_loader == MDKR_MOD_TEXTURE_LOADER_WORKER) loader_submit();
#endif
}

/* The usable packs that may serve `relative`, highest priority first, each
 * with the name it stores the file under, into `out`; how many. From the
 * registry's index, no I/O. A pack whose index could not be completed is
 * listed too, asked by the name as given. Owner thread; the spellings point
 * into the registry and are copied into the request. */
static int slot_candidates(const char *relative, TexCandidate *out, int max) {
    /* One hit per pack at most (mod_registry.h), so this never truncates:
     * every pack that holds the file is a candidate, down to the last. */
    MdkrModNameHit hits[MDKR_MOD_MAX_PACKS];
    const int      hit_count = mdkr_mod_registry_name_hits(
        s_registry, relative, hits, MDKR_MOD_MAX_PACKS);
    int            count = 0;
    int            pack;

    for (pack = MDKR_MOD_MAX_PACKS - 1; pack >= 0 && count < max; pack--) {
        const uint64_t bit = UINT64_C(1) << pack;
        const char    *spelling = NULL;
        int            hit;

        if ((s_usable_packs & bit) == 0) continue;
        for (hit = 0; hit < hit_count; hit++) {
            if ((hits[hit].packs & bit) != 0) {
                spelling = hits[hit].name;
                break;
            }
        }
        if (spelling == NULL && (s_registry->names_unindexed & bit) != 0) {
            spelling = relative;
        }
        if (spelling == NULL) continue;
        out[count].pack = pack;
        out[count].path = spelling;
        count++;
    }
    return count;
}

/* An UNRESOLVED slot, decided as far as the owner can without I/O: absent
 * from the indexes, refused on what the index alone shows, or a request. */
static void slot_begin(StoreSlot *slot, int rice, uint32_t crc, int fmt,
                       int siz) {
    TexCandidate  candidates[3][MDKR_MOD_MAX_PACKS];
    const TexCandidate *part[3] = { candidates[0], candidates[1],
                                    candidates[2] };
    int           count[3] = { 0, 0, 0 };
    int           rice_all = 0;
    char          relative[MDKR_MOD_TEXTURE_KEY_CHARS + 32];
    uint64_t      mark = timing_now();
    TexRequest   *req;

    if (!rice) {
        /* No usable pack holds any digest-named texture -- every Rice pack --
         * so there is nothing to probe for. */
        if (!s_digest_capable) {
            slot->state = SLOT_ABSENT;
            return;
        }
        snprintf(relative, sizeof relative, "textures/%s.png", slot->digest);
        if (s_timing_clock != NULL) s_timing.probes++;
        count[0] = slot_candidates(relative, candidates[0],
                                   MDKR_MOD_MAX_PACKS);
        if (count[0] == 0) {
            if (s_timing_clock != NULL) {
                const uint64_t before = s_timing.open_ns;
                timing_lap(&s_timing.open_ns, &mark);
                s_timing.absent_ns += s_timing.open_ns - before;
                s_timing.absent++;
            }
            slot->state = SLOT_ABSENT;
            return;
        }
    } else {
        const MdkrModRiceTexture *found =
            mdkr_mod_registry_rice_lookup(s_registry, crc, fmt, siz);
        if (found == NULL) {
            slot->state = SLOT_ABSENT;
            return;
        }
        if (found->all == NULL && found->rgb == NULL) {
            /* An entry that indexes ONLY an _a half has no colour to show.
             * It used to fail in silence. */
            report_rejection(slot->digest,
                             "this pack supplies only an _a half for that "
                             "texture, with no colour to apply it to");
            slot->state = SLOT_REJECTED;
            return;
        }
        if (found->all != NULL) {
            rice_all = 1;
            count[0] = slot_candidates(found->all, candidates[0],
                                       MDKR_MOD_MAX_PACKS);
        } else {
            count[1] = slot_candidates(found->rgb, candidates[1],
                                       MDKR_MOD_MAX_PACKS);
            if (found->alpha != NULL) {
                count[2] = slot_candidates(found->alpha, candidates[2],
                                           MDKR_MOD_MAX_PACKS);
            }
        }
        if (count[rice_all ? 0 : 1] == 0) {
            report_rejection(slot->digest,
                             "the pack file it names could not be opened");
            slot->state = SLOT_REJECTED;
            return;
        }
    }
    timing_lap(&s_timing.open_ns, &mark);

    req = request_new(slot->digest, rice, rice_all, part, count);
    if (req == NULL) return;   /* still UNRESOLVED; the next miss retries */
    loader_enqueue(slot, req);
}

/* ------------------------------------------------------------------- API */

void mdkr_mod_texture_store_init(const MdkrModRegistry *registry) {
    int index;
    int count;

    mdkr_mod_texture_store_shutdown();

    s_registry = registry;
    s_enabled_packs = 0;
    count = mdkr_mod_registry_count(registry);
    for (index = 0; index < count; index++) {
        const MdkrModEntry *entry = mdkr_mod_registry_entry(registry, index);
        if (entry != NULL && entry->manifest.enabled) s_enabled_packs++;
    }
    s_rice_identities = mdkr_mod_registry_rice_count(registry);
    s_cache_limit = s_test_cache_limit != 0 ? s_test_cache_limit
                                            : MDKR_MOD_TEXTURE_CACHE_BYTES_MAX;
    s_inflight_limit = s_cache_limit / MDKR_MOD_TEXTURE_INFLIGHT_SHARE;
    s_load_generation++;
    if (registry != NULL) loader_start();
}

void mdkr_mod_texture_store_shutdown(void) {
    size_t index;

    /* First: the worker is joined and every request it held is freed before
     * a single slot goes away, so no completion can outlive its table. */
    loader_stop();
    s_load_generation++;

    /* Indexing a pack and USING it are different claims. This is the second
     * one, and it is the only number that proves a texture actually reached
     * the GPU through the override path this run. */
    if (s_rice_resident > 0) {
        fprintf(stderr,
                "[MODS] %d high-resolution texture%s used this run\n",
                s_rice_resident, s_rice_resident == 1 ? "" : "s");
    }
    s_rice_resident = 0;

    for (index = 0; index < s_slot_capacity; index++) {
        stbi_image_free(s_slots[index].rgba);
    }
    free(s_slots);
    s_slots = NULL;
    s_slot_capacity = 0;
    s_slot_count = 0;
    s_resident_bytes = 0;
    s_use_clock = 0;
    s_reports = 0;
    s_pending = 0;
    s_first_resident = 0;
    s_first_resident_bytes = 0;
    s_reloads = 0;
    s_evictions = 0;
    s_registry = NULL;
    s_enabled_packs = 0;
    s_rice_identities = 0;
    s_settled_serial++;
    /* s_enabled and s_generation deliberately survive: the toggle is the
     * player's, not the registry's, and a generation that went backwards would
     * let a stale cache entry from before the reload look current. */
}

bool mdkr_mod_texture_store_active(void) {
    return s_enabled && s_registry != NULL && s_enabled_packs > 0;
}

static int texture_lookup_exact(const char *digest_hex, MdkrModTexture *out) {
    StoreSlot *slot;

    if (out != NULL) {
        out->rgba = NULL;
        out->width = 0;
        out->height = 0;
        out->pending = 0;
    }
    if (!mdkr_mod_texture_store_active()) return 0;
    if (out == NULL || digest_hex == NULL) return 0;
    /* Anything that is not a digest this store names cannot have a file, and
     * letting it through would let a caller's bug become a probe. */
    if (strlen(digest_hex) != MDKR_MOD_TEXTURE_DIGEST_CHARS) return 0;

    slot = slot_for(digest_hex);
    if (slot == NULL) return 0;
    if (slot->state == SLOT_UNRESOLVED) {
        const uint64_t start = timing_now();
        slot_begin(slot, 0, 0u, 0, 0);
        if (s_timing_clock != NULL) {
            s_timing.resolve_ns += timing_since(start);
            s_timing.resolves++;
        }
    }
    if (slot->state == SLOT_PENDING) {
        out->pending = 1;
        return 0;
    }
    if (slot->state != SLOT_RESIDENT) return 0;

    slot->last_use = ++s_use_clock;
    out->rgba = slot->rgba;
    out->width = slot->width;
    out->height = slot->height;
    return 1;
}

int mdkr_mod_texture_lookup(const char *digest_hex, MdkrModTexture *out) {
    if (texture_lookup_exact(digest_hex, out)) return 1;
    /* The current name is still loading: it wins if it turns out usable, so
     * the legacy name is not consulted until it has settled. */
    if (out != NULL && out->pending) return 0;
    /* Issue #58 retouches only our generated Taj card. A valid current-name
     * replacement wins; otherwise retain the published pre-1.7 pack name.
     * Both paths use the same enable, validation, caching, and size limits.
     * Never rewrite the content digest itself: author dumps must still name
     * the pixels they actually saw, and unrelated textures remain unchanged. */
    if (digest_hex != NULL &&
        strcmp(digest_hex, MDKR_MOD_TAJ_PORTRAIT_DIGEST) == 0) {
        return texture_lookup_exact(MDKR_MOD_TAJ_PORTRAIT_LEGACY_DIGEST, out);
    }
    return 0;
}

int mdkr_mod_texture_rice_resident(void) { return s_rice_resident; }

void mdkr_mod_texture_memory_stats(MdkrModTextureMemoryStats *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    out->pixel_bytes = s_resident_bytes;
    out->pixel_limit = s_cache_limit;
    out->table_bytes = s_slot_capacity * sizeof(*s_slots);
    out->entries = s_slot_count;
    out->pending = s_pending;
    out->first_resident = s_first_resident;
    out->first_resident_bytes = s_first_resident_bytes;
    out->reloads = s_reloads;
    out->evictions = s_evictions;
#if STORE_HAS_WORKER
    if (s_shared != NULL) {
        SDL_LockMutex(s_shared->lock);
        out->inflight_bytes = s_shared->inflight;
        SDL_UnlockMutex(s_shared->lock);
    }
#endif
}

int mdkr_mod_texture_rice_active(void) {
    return mdkr_mod_texture_store_active() && s_rice_identities > 0;
}

int mdkr_mod_texture_lookup_rice(uint32_t crc, int fmt, int siz,
                                 MdkrModTexture *out) {
    char       key[MDKR_MOD_TEXTURE_KEY_CHARS + 1];
    StoreSlot *slot;

    if (out != NULL) {
        out->rgba = NULL; out->width = 0; out->height = 0; out->pending = 0;
    }
    if (!mdkr_mod_texture_store_active()) return 0;
    if (s_rice_identities == 0) return 0;

    /* The slot table is keyed by string, so a Rice identity gets one that
     * cannot collide with a 32-character content digest. */
    snprintf(key, sizeof key, "rice:%08x:%d:%d", (unsigned)crc, fmt, siz);
    slot = slot_for(key);
    if (slot == NULL) return 0;
    if (slot->state == SLOT_UNRESOLVED) {
        const uint64_t start = timing_now();
        slot_begin(slot, 1, crc, fmt, siz);
        if (s_timing_clock != NULL) {
            s_timing.resolve_ns += timing_since(start);
            s_timing.resolves++;
        }
    }
    if (slot->state == SLOT_PENDING) {
        if (out != NULL) out->pending = 1;
        return 0;
    }
    if (slot->state != SLOT_RESIDENT) return 0;

    slot->last_use = ++s_use_clock;
    if (out != NULL) {
        out->rgba = slot->rgba;
        out->width = slot->width;
        out->height = slot->height;
    }
    return 1;
}

void mdkr_mod_texture_store_set_loader(MdkrModTextureLoader loader) {
    s_loader_request = loader;
}

MdkrModTextureLoader mdkr_mod_texture_store_loader(void) {
    return s_loader;
}

void mdkr_mod_texture_store_pump(const MdkrModTexturePumpBudget *budget,
                                 MdkrModTextureClock clock) {
    static const MdkrModTexturePumpBudget defaults = {
        MDKR_MOD_TEXTURE_INSTALL_COUNT, MDKR_MOD_TEXTURE_INSTALL_BYTES,
        MDKR_MOD_TEXTURE_WORK_NS
    };
    const uint64_t timing_start = timing_now();

    if (budget == NULL) budget = &defaults;
    s_pump_count++;
#if STORE_HAS_WORKER
    if (s_loader == MDKR_MOD_TEXTURE_LOADER_WORKER) {
        loader_drain(budget->install_count, budget->install_bytes, 0);
        loader_submit();
    }
#endif
    if (s_loader == MDKR_MOD_TEXTURE_LOADER_BUDGET && s_wait_head != NULL) {
        const uint64_t start = clock != NULL ? clock() : 0u;
        uint32_t       count = 0;
        size_t         bytes = 0;
        while (s_wait_head != NULL) {
            if (count > 0) {
                if (count >= budget->install_count ||
                    bytes >= budget->install_bytes || clock == NULL) {
                    break;
                }
                if (clock() - start >= budget->work_ns) break;
            }
            bytes += owner_run_one();
            count++;
        }
    }
    if (s_timing_clock != NULL) s_timing.pump_ns += timing_since(timing_start);
}

void mdkr_mod_texture_store_settle(void) {
    const uint64_t timing_start = timing_now();

    if (s_loader == MDKR_MOD_TEXTURE_LOADER_BUDGET) {
        while (s_wait_head != NULL) (void)owner_run_one();
    }
#if STORE_HAS_WORKER
    if (s_loader == MDKR_MOD_TEXTURE_LOADER_WORKER && s_shared != NULL) {
        for (;;) {
            loader_submit();
            if (s_outstanding == 0 && s_wait_head == NULL) break;
            SDL_LockMutex(s_shared->lock);
            while (s_shared->done_head == NULL) {
                SDL_CondWait(s_shared->wake_owner, s_shared->lock);
            }
            SDL_UnlockMutex(s_shared->lock);
            loader_drain(0u, 0u, 1);
        }
    }
#endif
    if (s_timing_clock != NULL) s_timing.pump_ns += timing_since(timing_start);
}

uint32_t mdkr_mod_texture_settled_serial(void) {
    return s_settled_serial;
}

void mdkr_mod_texture_set_install_observer(
    MdkrModTextureInstallObserver observer) {
    s_observer = observer;
}

void mdkr_mod_texture_set_enabled(bool enabled) {
    if (s_enabled == enabled) return;
    s_enabled = enabled;
    /* Every cached GPU texture carries the generation it was uploaded under, so
     * bumping it retires both variants' entries without touching either. The
     * decoded pixels stay: the whole point of the toggle is that switching back
     * costs a bind, not a re-decode. What is still loading is retired: it was
     * asked for under the old setting. */
    s_generation++;
    loader_retarget();
}

bool mdkr_mod_texture_enabled(void) {
    return s_enabled;
}

void mdkr_mod_texture_timing_set_clock(MdkrModTextureClock clock) {
    s_timing_clock = clock;
}

void mdkr_mod_texture_timing_totals(MdkrModTextureResolveTiming *out) {
    if (out != NULL) *out = s_timing;
}

uint32_t mdkr_mod_texture_generation(void) {
    return s_generation;
}

/* ------------------------------------------------------------ test seams */

#ifdef MDKR_MOD_TEXTURE_STORE_TESTING
void mdkr_mod_texture_store_test_fail_worker_start(int fail) {
#if STORE_HAS_WORKER
    s_test_fail_worker = fail;
#else
    (void)fail;
#endif
}

void mdkr_mod_texture_store_test_set_cache_limit(size_t bytes) {
    s_test_cache_limit = bytes;
}

void mdkr_mod_texture_store_test_hold_worker(int hold) {
#if STORE_HAS_WORKER
    s_test_hold = hold;
    if (s_shared != NULL) {
        SDL_LockMutex(s_shared->lock);
        s_shared->hold = hold;
        SDL_CondBroadcast(s_shared->wake_worker);
        SDL_UnlockMutex(s_shared->lock);
    }
#else
    (void)hold;
#endif
}

int mdkr_mod_texture_store_test_wait_parked(void) {
#if STORE_HAS_WORKER
    if (s_shared == NULL) return 0;
    SDL_LockMutex(s_shared->lock);
    while (!s_shared->parked) {
        SDL_CondWait(s_shared->wake_owner, s_shared->lock);
    }
    SDL_UnlockMutex(s_shared->lock);
    return 1;
#else
    return 0;
#endif
}

int mdkr_mod_texture_store_test_wait_idle(void) {
#if STORE_HAS_WORKER
    if (s_shared == NULL) return 0;
    loader_submit();
    SDL_LockMutex(s_shared->lock);
    while (s_shared->busy || s_shared->req_head != NULL) {
        SDL_CondWait(s_shared->wake_owner, s_shared->lock);
    }
    SDL_UnlockMutex(s_shared->lock);
    return 1;
#else
    return 0;
#endif
}
#endif

/* ---------------------------------------------- author dump (Task 9) ---- */

/* Read once and cached: every miss-path texture bind would otherwise pay a
 * getenv() call for a variable that is either absent for the entire process
 * or present for the entire process. NULL means "not set" or "set empty",
 * which this store treats the same. */
static const char *dump_directory(void) {
    static int         resolved;
    static const char *directory;

    if (!resolved) {
        const char *env = getenv("MDKR_MOD_TEXTURE_DUMP");
        directory = (env != NULL && env[0] != '\0') ? env : NULL;
        resolved = 1;
    }
    return directory;
}

bool mdkr_mod_texture_dump_active(void) {
    return dump_directory() != NULL;
}

/* Digests already written this process. A flat array with a linear scan, not
 * a hash table: dump mode is off by default, is never the hot path even when
 * on (the texture cache's own miss rate bounds how often a digest is ever
 * offered here), and a session's distinct texture count is a few thousand at
 * most -- nowhere near where the scan would be felt.
 *
 * Deliberately NOT cleared by mdkr_mod_texture_store_shutdown(), for the same
 * reason s_enabled and s_generation are not: it answers a question about the
 * process's lifetime ("has this digest reached disk yet"), not the
 * registry's, and a settings-triggered store reinit must not put the same PNG
 * on disk twice. */
static char (*s_dump_seen)[MDKR_MOD_TEXTURE_DIGEST_CHARS + 1];
static size_t s_dump_seen_count;
static size_t s_dump_seen_capacity;

/* Records `digest` as seen and returns true the first time it is asked
 * about; false on every later call for the same digest. Returns true (never
 * blocks the write) when the array cannot grow -- a failed allocation here
 * should risk a duplicate write, not a lost one. */
static bool dump_mark_seen(const char *digest) {
    size_t index;

    for (index = 0; index < s_dump_seen_count; index++) {
        if (strcmp(s_dump_seen[index], digest) == 0) return false;
    }
    if (s_dump_seen_count == s_dump_seen_capacity) {
        size_t next = s_dump_seen_capacity == 0 ? 64u : s_dump_seen_capacity * 2u;
        void  *grown = realloc(s_dump_seen, next * sizeof(*s_dump_seen));
        if (grown == NULL) return true;
        s_dump_seen = (char (*)[MDKR_MOD_TEXTURE_DIGEST_CHARS + 1])grown;
        s_dump_seen_capacity = next;
    }
    memcpy(s_dump_seen[s_dump_seen_count], digest, strlen(digest) + 1);
    s_dump_seen_count++;
    return true;
}

/* Distinct dump failures written to stderr before it stops listing them. A
 * counter separate from report_rejection()'s budget: a broken dump directory
 * and a broken pack PNG are unrelated failures, and one filling its budget
 * must not silence the other. */
#define MDKR_MOD_TEXTURE_DUMP_REPORT_MAX 8
static int s_dump_reports;

static void report_dump_failure(const char *digest, const char *path,
                                const char *reason) {
    if (s_dump_reports >= MDKR_MOD_TEXTURE_DUMP_REPORT_MAX) return;
    s_dump_reports++;
    fprintf(stderr, "[MODS] dump %s -> %s: %s\n", digest, path, reason);
    if (s_dump_reports == MDKR_MOD_TEXTURE_DUMP_REPORT_MAX) {
        fprintf(stderr, "[MODS] further dump failures will not be listed\n");
    }
}

static bool dump_write_bytes(const char *path, const void *bytes, size_t length) {
    FILE  *file = store_open_write(path);
    size_t written;

    if (file == NULL) return false;
    written = fwrite(bytes, 1, length, file);
    fclose(file);
    return written == length;
}

/* Accumulates stbi_write_png_to_func()'s callback chunks into one buffer, so
 * the encoded PNG can be handed to dump_write_bytes() -- the same UTF-8-safe
 * fopen path read_whole_file() uses to read a pack -- instead of the header's
 * own file-writing entry points, which STBI_WRITE_NO_STDIO above removes for
 * exactly this reason. */
typedef struct DumpPngBuffer {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
    bool     failed;
} DumpPngBuffer;

static void dump_png_write_cb(void *context, void *chunk, int size) {
    DumpPngBuffer *buffer = (DumpPngBuffer *)context;
    size_t         needed;

    if (buffer->failed || size <= 0) return;
    if ((size_t)size > SIZE_MAX - buffer->size) {
        buffer->failed = true;
        return;
    }
    needed = buffer->size + (size_t)size;
    if (needed > buffer->capacity) {
        size_t   next = buffer->capacity == 0 ? 65536u : buffer->capacity;
        uint8_t *grown;
        while (next < needed) {
            if (next > SIZE_MAX / 2u) {
                next = needed;
                break;
            }
            next *= 2u;
        }
        grown = (uint8_t *)realloc(buffer->data, next);
        if (grown == NULL) {
            buffer->failed = true;
            return;
        }
        buffer->data = grown;
        buffer->capacity = next;
    }
    memcpy(buffer->data + buffer->size, chunk, (size_t)size);
    buffer->size += (size_t)size;
}

/* True when `source` carries a span worth writing. A source with no bytes is
 * not an error -- a caller that had none to offer says so with a null, and a
 * zero-length span is the same statement -- so both write the record without
 * its source fields and no `.texels` beside it. A reader keys on that file
 * existing, so the pair is refused as unusable rather than half-read. */
static bool dump_source_has_span(const MdkrModTextureSource *source) {
    return source != NULL && source->texels != NULL && source->texel_bytes > 0;
}

void mdkr_mod_texture_dump_observe(const char *digest_hex, const uint8_t *rgba,
                                   int width, int height, uint8_t fmt,
                                   uint8_t siz,
                                   const MdkrModTextureSource *source,
                                   const char *first_seen) {
    const char   *dir = dump_directory();
    char          png_path[MDKR_MOD_PATH_MAX];
    char          txt_path[MDKR_MOD_PATH_MAX];
    char          texels_path[MDKR_MOD_PATH_MAX];
    /* Eleven fields, of which first_seen is the only unbounded one and its
     * one caller writes about forty characters. Sized so the record cannot be
     * truncated in practice, and checked below so it cannot be truncated
     * silently if it ever is. */
    char          txt_body[512];
    int           txt_length;
    DumpPngBuffer png = { NULL, 0, 0, false };
    MdkrPngWriteLayout layout;

    if (dir == NULL) return;
    if (digest_hex == NULL || rgba == NULL) return;
    if (strlen(digest_hex) != MDKR_MOD_TEXTURE_DIGEST_CHARS) return;
    if (!mdkr_png_write_layout(width, height, 4, &layout)) {
        report_dump_failure(digest_hex, dir, "the image exceeds PNG encoder limits");
        return;
    }
    if (!dump_mark_seen(digest_hex)) return; /* already written this run */

    if (snprintf(png_path, sizeof png_path, "%s/%s.png", dir, digest_hex) >=
            (int)sizeof png_path ||
        snprintf(txt_path, sizeof txt_path, "%s/%s.txt", dir, digest_hex) >=
            (int)sizeof txt_path ||
        snprintf(texels_path, sizeof texels_path, "%s/%s.texels", dir,
                 digest_hex) >= (int)sizeof texels_path) {
        report_dump_failure(digest_hex, dir, "the dump path is too long");
        return;
    }

    if (!stbi_write_png_to_func(dump_png_write_cb, &png, width, height, 4,
                                rgba, (int)layout.row_bytes) ||
        png.failed || png.data == NULL) {
        free(png.data);
        report_dump_failure(digest_hex, png_path, "the PNG could not be encoded");
        return;
    }
    if (!dump_write_bytes(png_path, png.data, png.size)) {
        free(png.data);
        report_dump_failure(digest_hex, png_path, "the file could not be written");
        return;
    }
    free(png.data);

    /* The raw span goes down before the record that describes it. A reader
     * keys on the `.texels` file existing, so writing the record first would
     * leave a window -- and, if the span write then failed, a permanent
     * record -- claiming bytes that are not there. */
    if (dump_source_has_span(source) &&
        !dump_write_bytes(texels_path, source->texels, source->texel_bytes)) {
        report_dump_failure(digest_hex, texels_path,
                            "the texel span could not be written");
        return;
    }

    if (dump_source_has_span(source)) {
        txt_length = snprintf(
            txt_body, sizeof txt_body,
            "dump_format=%u\nwidth=%d\nheight=%d\nfmt=%u\nsiz=%u\n"
            "source_width=%d\nsource_height=%d\nsource_line_bytes=%u\n"
            "source_size_bytes=%u\nsource_texel_bytes=%zu\nfirst_seen=%s\n",
            MDKR_MOD_TEXTURE_DUMP_FORMAT, width, height, (unsigned)fmt,
            (unsigned)siz, source->width, source->height,
            (unsigned)source->line_bytes, (unsigned)source->size_bytes,
            source->texel_bytes, first_seen != NULL ? first_seen : "");
    } else {
        txt_length = snprintf(
            txt_body, sizeof txt_body,
            "dump_format=%u\nwidth=%d\nheight=%d\nfmt=%u\nsiz=%u\n"
            "first_seen=%s\n",
            MDKR_MOD_TEXTURE_DUMP_FORMAT, width, height, (unsigned)fmt,
            (unsigned)siz, first_seen != NULL ? first_seen : "");
    }
    if (txt_length < 0) return;
    /* Refused rather than truncated. This record is parsed by a tool, and a
     * cut-off `key=value` line is not a shorter record -- it is a wrong one
     * (`source_line_bytes=12` where the value was 128), which is exactly the
     * failure this whole path exists to avoid. */
    if ((size_t)txt_length >= sizeof txt_body) {
        report_dump_failure(digest_hex, txt_path,
                            "the record does not fit its buffer");
        return;
    }
    if (!dump_write_bytes(txt_path, txt_body, (size_t)txt_length)) {
        report_dump_failure(digest_hex, txt_path, "the file could not be written");
    }
}
