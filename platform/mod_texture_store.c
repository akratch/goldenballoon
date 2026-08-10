/**
 * mod_texture_store.c — see mod_texture_store.h.
 *
 * The store is one open-addressed table keyed by the 32-hex content digest.
 * Every digest the renderer ever asks about gets a slot, including the ones no
 * pack provides, because the overwhelmingly common answer is "no" and answering
 * it from a filesystem probe every time a texture missed the GPU cache would
 * cost far more than the slot does. A slot is therefore a decision that has
 * already been made, not just a cache of pixels:
 *
 *   SLOT_UNRESOLVED — the digest is known, its answer is not yet (a fresh slot,
 *                     or one whose pixels were evicted and must be decoded
 *                     again if it is wanted).
 *   SLOT_ABSENT     — no enabled pack holds textures/<digest>.png.
 *   SLOT_REJECTED   — a pack holds it, but it could not be used. Already
 *                     reported, once, and never retried.
 *   SLOT_RESIDENT   — decoded RGBA8 held in `rgba`.
 *
 * Only SLOT_RESIDENT ever transitions backwards (to SLOT_UNRESOLVED, when it is
 * evicted). That is what makes "report a bad PNG once" true by construction
 * rather than by a separate flag that could drift out of step with the state.
 *
 * Eviction picks the least recently used resident slot by a linear scan. That
 * is O(table) per eviction, deliberately: eviction only happens once the cache
 * is holding half a gigabyte of pixels, and a scan of a few thousand slots at
 * that point is invisible next to the PNG decode that triggered it. A heap or
 * an intrusive LRU list would be more code to be wrong in for no measurable
 * gain.
 *
 * Single-threaded; see the header. No lock, by standing decision.
 */
#include "mod_texture_store.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pack content is read through mod_source, never through a path this file
 * opens itself. That is what makes a zipped pack and an unpacked one the same
 * pack here: this module asks the registry for whichever one holds the digest
 * and reads bytes back, with no branch on which kind answered. It also keeps
 * the Windows UTF-8/long-path boundary in exactly one place (fs_utf8, reached
 * through mod_source) rather than giving this file a second one.
 *
 * The author-dump path below still writes files, and that is the only reason
 * fs_utf8 is included here at all. */
#include "mod_source.h"
#if defined(_WIN32)
#include "fs_utf8.h"
#endif

/* stb_image is instantiated in exactly one translation unit,
 * lib/stb/stb_image_impl.c, which is compiled with warnings off. This TU takes
 * the declarations only, and must configure the header identically or the two
 * would disagree about which entry points exist. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* stb_image_write, same arrangement: instantiated once, in the same TU as the
 * decoder, and taken here as declarations only. STBI_WRITE_NO_STDIO removes
 * the header's own fopen()-based entry points, which is what makes the *_to_func
 * callback the only way to reach the encoder from this file -- the same reason
 * STBI_NO_STDIO is set above, so the dump path writes through the port's own
 * UTF-8-safe fopen (mdkr_fopen_utf8 on Windows) rather than a second, narrower
 * one the header would otherwise carry. */
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* Total decoded pixels held at once. A 4K RGBA texture is 64 MiB, so this is
 * eight of them — generous for a pack that replaces the HUD and a few tracks,
 * and a hard stop for one that replaces everything at 4K. */
#define MDKR_MOD_TEXTURE_CACHE_BYTES_MAX ((size_t)512u * 1024u * 1024u)

/* Largest PNG file this will read into memory before handing it to the decoder.
 * A legitimate 4K RGBA PNG compresses to a few tens of megabytes; anything past
 * this is either not a texture or is not one this store is willing to hold. */
#define MDKR_MOD_TEXTURE_FILE_BYTES_MAX ((size_t)64u * 1024u * 1024u)

/* Length of the digest the renderer addresses textures by. Fixed by
 * mdkr_mod_texture_digest(), which is a published contract. */
#define MDKR_MOD_TEXTURE_DIGEST_CHARS 32

/* Distinct rejections written to the log before it stops listing them. Each
 * rejection is reported once, so this only matters for a pack that is broken
 * wholesale — and in that case the first few lines say everything the next
 * thousand would. */
#define MDKR_MOD_TEXTURE_REPORT_MAX 32

#define MDKR_MOD_TEXTURE_TABLE_MIN 256u

enum {
    SLOT_FREE = 0,
    SLOT_UNRESOLVED,
    SLOT_ABSENT,
    SLOT_REJECTED,
    SLOT_RESIDENT
};

typedef struct StoreSlot {
    char      digest[MDKR_MOD_TEXTURE_DIGEST_CHARS + 1];
    uint8_t  *rgba;
    int       width;
    int       height;
    size_t    bytes;
    uint64_t  last_use;
    int       state;
} StoreSlot;

static const MdkrModRegistry *s_registry;
static int       s_enabled_packs;
static bool      s_enabled = true;
static uint32_t  s_generation;

static StoreSlot *s_slots;
static size_t     s_slot_capacity;   /* always a power of two, or 0 */
static size_t     s_slot_count;      /* slots not in SLOT_FREE */
static size_t     s_resident_bytes;
static uint64_t   s_use_clock;
static int        s_reports;

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
        fresh->rgba = old->rgba;
        fresh->width = old->width;
        fresh->height = old->height;
        fresh->bytes = old->bytes;
        fresh->last_use = old->last_use;
        fresh->state = old->state;
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

/* Drops least-recently-used residents until `incoming` more bytes fit under the
 * cap. Returns 0 when even an empty cache could not hold it. */
static int evict_for(size_t incoming) {
    if (incoming > MDKR_MOD_TEXTURE_CACHE_BYTES_MAX) return 0;

    while (s_resident_bytes + incoming > MDKR_MOD_TEXTURE_CACHE_BYTES_MAX) {
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

/* Reads the whole of `file`'s entry into a fresh buffer. Returns NULL on any
 * failure, with a player-readable reason — including a file past this store's
 * own ceiling, which is well under mod_source's, because a PNG that big is not
 * a texture whatever the container is willing to hand over.
 *
 * The size is asked for first and the buffer sized from the answer, which is
 * safe only because mod_source has already bounded that answer: it refuses any
 * entry over MDKR_MOD_SOURCE_ENTRY_MAX before reporting a size at all, and the
 * read below refuses to write anything unless the whole entry fits. A declared
 * length is never allocated from on this path. */
static unsigned char *read_pack_entry(MdkrModFile *file, size_t *out_size,
                                      const char **out_reason) {
    unsigned char *buffer;
    size_t         size = 0;
    int            result;

    *out_size = 0;
    *out_reason = NULL;

    result = mdkr_mod_source_read(file->source, file->relative, NULL, 0, &size);
    if (result != MDKR_MOD_SOURCE_BUFFER_TOO_SMALL &&
        result != MDKR_MOD_SOURCE_OK) {
        *out_reason = mdkr_mod_source_result_text(result);
        return NULL;
    }
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
    result = mdkr_mod_source_read(file->source, file->relative, buffer, size,
                                  &size);
    if (result != MDKR_MOD_SOURCE_OK) {
        free(buffer);
        *out_reason = mdkr_mod_source_result_text(result);
        return NULL;
    }
    *out_size = size;
    return buffer;
}

/* ------------------------------------------------------------- resolution */

static void slot_resolve(StoreSlot *slot) {
    char           relative[MDKR_MOD_TEXTURE_DIGEST_CHARS + 32];
    MdkrModFile    file;
    const char    *reason = NULL;
    unsigned char *file_bytes;
    size_t         file_size = 0;
    unsigned char *pixels;
    int            declared_width = 0;
    int            declared_height = 0;
    int            declared_channels = 0;
    int            width = 0;
    int            height = 0;
    int            channels = 0;
    size_t         decoded;

    snprintf(relative, sizeof relative, "textures/%s.png", slot->digest);
    /* Directory pack or zip pack — the store cannot tell and must not care.
     * One open per newly resolved digest, and a slot resolves once. */
    if (!mdkr_mod_registry_open_file(s_registry, relative, &file)) {
        slot->state = SLOT_ABSENT;
        return;
    }

    file_bytes = read_pack_entry(&file, &file_size, &reason);
    mdkr_mod_registry_close_file(&file);
    if (file_bytes == NULL) {
        slot->state = SLOT_REJECTED;
        report_rejection(slot->digest, reason);
        return;
    }
    /* The cap above is far below INT_MAX, so this only documents the bound the
     * decoder's int-typed length depends on. */
    if (file_size > (size_t)INT_MAX) {
        free(file_bytes);
        slot->state = SLOT_REJECTED;
        report_rejection(slot->digest, "the file is too large to decode");
        return;
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
     * Measured in 64 bits and against the same cap evict_for() enforces, so
     * the two cannot drift apart. The declared component count is deliberately
     * ignored: this store always asks the decoder for RGBA, so four bytes a
     * pixel is what a decode of this file would cost whatever the file itself
     * stores -- and a one-channel image is precisely the case where the two
     * differ by four.
     *
     * A header that cannot be parsed at all falls through to the decode on
     * purpose. The decoder parses the same header and will fail on it with
     * stb's own wording, which names the defect better than this module could
     * about a header it could not read; and every check after the decode still
     * runs regardless, because stbi_info() reports what the header claims and
     * a header that lies has to be caught by the decode itself. */
    if (stbi_info_from_memory(file_bytes, (int)file_size, &declared_width,
                              &declared_height, &declared_channels) != 0 &&
        declared_width > 0 && declared_height > 0) {
        uint64_t declared = (uint64_t)declared_width *
                            (uint64_t)declared_height * 4u;
        if (declared > (uint64_t)MDKR_MOD_TEXTURE_CACHE_BYTES_MAX) {
            char too_big[128];
            snprintf(too_big, sizeof too_big,
                     "the image declares %dx%d, larger than the whole texture "
                     "cache", declared_width, declared_height);
            free(file_bytes);
            slot->state = SLOT_REJECTED;
            report_rejection(slot->digest, too_big);
            return;
        }
    }

    pixels = stbi_load_from_memory(file_bytes, (int)file_size, &width, &height,
                                   &channels, 4);
    free(file_bytes);
    if (pixels == NULL) {
        /* stb's own wording, so the log names the actual defect in the PNG
         * rather than this module's guess at it. */
        slot->state = SLOT_REJECTED;
        report_rejection(slot->digest, stbi_failure_reason());
        return;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        slot->state = SLOT_REJECTED;
        report_rejection(slot->digest, "the image has no pixels");
        return;
    }

    decoded = (size_t)width * (size_t)height * 4u;
    if (!evict_for(decoded)) {
        stbi_image_free(pixels);
        slot->state = SLOT_REJECTED;
        report_rejection(slot->digest,
                         "the image is larger than the whole texture cache");
        return;
    }

    slot->rgba = (uint8_t *)pixels;
    slot->width = width;
    slot->height = height;
    slot->bytes = decoded;
    slot->state = SLOT_RESIDENT;
    s_resident_bytes += decoded;
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
}

void mdkr_mod_texture_store_shutdown(void) {
    size_t index;

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
    s_registry = NULL;
    s_enabled_packs = 0;
    /* s_enabled and s_generation deliberately survive: the toggle is the
     * player's, not the registry's, and a generation that went backwards would
     * let a stale cache entry from before the reload look current. */
}

bool mdkr_mod_texture_store_active(void) {
    return s_enabled && s_registry != NULL && s_enabled_packs > 0;
}

int mdkr_mod_texture_lookup(const char *digest_hex, MdkrModTexture *out) {
    StoreSlot *slot;

    if (out != NULL) {
        out->rgba = NULL;
        out->width = 0;
        out->height = 0;
    }
    if (!mdkr_mod_texture_store_active()) return 0;
    if (out == NULL || digest_hex == NULL) return 0;
    /* Anything that is not a digest this store names cannot have a file, and
     * letting it through would let a caller's bug become a filesystem probe. */
    if (strlen(digest_hex) != MDKR_MOD_TEXTURE_DIGEST_CHARS) return 0;

    slot = slot_for(digest_hex);
    if (slot == NULL) return 0;
    if (slot->state == SLOT_UNRESOLVED) slot_resolve(slot);
    if (slot->state != SLOT_RESIDENT) return 0;

    slot->last_use = ++s_use_clock;
    out->rgba = slot->rgba;
    out->width = slot->width;
    out->height = slot->height;
    return 1;
}

void mdkr_mod_texture_set_enabled(bool enabled) {
    if (s_enabled == enabled) return;
    s_enabled = enabled;
    /* Every cached GPU texture carries the generation it was uploaded under, so
     * bumping it retires both variants' entries without touching either. The
     * decoded pixels stay: the whole point of the toggle is that switching back
     * costs a bind, not a re-decode. */
    s_generation++;
}

bool mdkr_mod_texture_enabled(void) {
    return s_enabled;
}

uint32_t mdkr_mod_texture_generation(void) {
    return s_generation;
}

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
    needed = buffer->size + (size_t)size;
    if (needed > buffer->capacity) {
        size_t   next = buffer->capacity == 0 ? 65536u : buffer->capacity;
        uint8_t *grown;
        while (next < needed) next *= 2u;
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

void mdkr_mod_texture_dump_observe(const char *digest_hex, const uint8_t *rgba,
                                   int width, int height, uint8_t fmt,
                                   uint8_t siz, const char *first_seen) {
    const char   *dir = dump_directory();
    char          png_path[MDKR_MOD_PATH_MAX];
    char          txt_path[MDKR_MOD_PATH_MAX];
    char          txt_body[256];
    int           txt_length;
    DumpPngBuffer png = { NULL, 0, 0, false };

    if (dir == NULL) return;
    if (digest_hex == NULL || rgba == NULL) return;
    if (width <= 0 || height <= 0) return;
    if (strlen(digest_hex) != MDKR_MOD_TEXTURE_DIGEST_CHARS) return;
    if (!dump_mark_seen(digest_hex)) return; /* already written this run */

    if (snprintf(png_path, sizeof png_path, "%s/%s.png", dir, digest_hex) >=
            (int)sizeof png_path ||
        snprintf(txt_path, sizeof txt_path, "%s/%s.txt", dir, digest_hex) >=
            (int)sizeof txt_path) {
        report_dump_failure(digest_hex, dir, "the dump path is too long");
        return;
    }

    if (!stbi_write_png_to_func(dump_png_write_cb, &png, width, height, 4,
                                rgba, width * 4) ||
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

    txt_length = snprintf(txt_body, sizeof txt_body,
                          "width=%d\nheight=%d\nfmt=%u\nsiz=%u\nfirst_seen=%s\n",
                          width, height, (unsigned)fmt, (unsigned)siz,
                          first_seen != NULL ? first_seen : "");
    if (txt_length < 0) return;
    if ((size_t)txt_length >= sizeof txt_body) txt_length = (int)sizeof(txt_body) - 1;
    if (!dump_write_bytes(txt_path, txt_body, (size_t)txt_length)) {
        report_dump_failure(digest_hex, txt_path, "the file could not be written");
    }
}
