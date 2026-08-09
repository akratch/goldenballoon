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

#if defined(_WIN32)
/* Paths reach this module from the registry, which built them out of directory
 * names the OS gave it, so they can contain anything a filesystem allows. The
 * narrow CRT would read them in the active code page and stop at MAX_PATH;
 * fs_utf8 already owns that boundary for the whole port. */
#include "fs_utf8.h"
#endif

/* stb_image is instantiated in exactly one translation unit,
 * lib/stb/stb_image_impl.c, which is compiled with warnings off. This TU takes
 * the declarations only, and must configure the header identically or the two
 * would disagree about which entry points exist. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

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
static FILE *store_open_read(const char *path) {
    return mdkr_fopen_utf8(path, "rb");
}
#else
static FILE *store_open_read(const char *path) {
    return fopen(path, "rb");
}
#endif

/* Reads the whole file. Returns NULL on any failure, including a file past the
 * size cap — a caller cannot distinguish those and does not need to, since both
 * end the same way: this digest has no usable override. */
static unsigned char *read_whole_file(const char *path, size_t *out_size,
                                      const char **out_reason) {
    FILE          *file;
    unsigned char *buffer;
    size_t         capacity = 0;
    size_t         filled = 0;

    *out_size = 0;
    *out_reason = NULL;

    file = store_open_read(path);
    if (file == NULL) {
        *out_reason = "the file could not be opened";
        return NULL;
    }

    /* Read incrementally rather than trusting a seek-derived length: the size
     * cap has to hold even if the file grows, or is a pipe, or reports a length
     * it does not have. */
    buffer = NULL;
    for (;;) {
        size_t want;
        size_t got;

        if (filled == capacity) {
            unsigned char *grown;
            size_t next = capacity == 0 ? 65536u : capacity * 2u;
            if (next > MDKR_MOD_TEXTURE_FILE_BYTES_MAX) {
                next = MDKR_MOD_TEXTURE_FILE_BYTES_MAX;
            }
            if (next == capacity) {
                *out_reason = "the file is too large to load";
                goto failed;
            }
            grown = (unsigned char *)realloc(buffer, next);
            if (grown == NULL) {
                *out_reason = "there was not enough memory to load it";
                goto failed;
            }
            buffer = grown;
            capacity = next;
        }
        want = capacity - filled;
        got = fread(buffer + filled, 1, want, file);
        filled += got;
        if (got < want) {
            if (ferror(file)) {
                *out_reason = "the file could not be read";
                goto failed;
            }
            break; /* end of file */
        }
    }

    fclose(file);
    if (filled == 0) {
        free(buffer);
        *out_reason = "the file is empty";
        return NULL;
    }
    *out_size = filled;
    return buffer;

failed:
    fclose(file);
    free(buffer);
    return NULL;
}

/* ------------------------------------------------------------- resolution */

static void slot_resolve(StoreSlot *slot) {
    char           relative[MDKR_MOD_TEXTURE_DIGEST_CHARS + 32];
    char           path[MDKR_MOD_PATH_MAX];
    const char    *reason = NULL;
    unsigned char *file_bytes;
    size_t         file_size = 0;
    unsigned char *pixels;
    int            width = 0;
    int            height = 0;
    int            channels = 0;
    size_t         decoded;

    snprintf(relative, sizeof relative, "textures/%s.png", slot->digest);
    if (!mdkr_mod_registry_resolve(s_registry, relative, path, sizeof path)) {
        slot->state = SLOT_ABSENT;
        return;
    }

    file_bytes = read_whole_file(path, &file_size, &reason);
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
