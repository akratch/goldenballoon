/**
 * mod_registry.c — see mod_registry.h.
 */
#include "mod_registry.h"
#include "mod_manifest.h"
#include "mod_source.h"
#include "fs_utf8.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Enumeration is the same code on every platform: the mods folder is listed
 * through mdkr_directory_names_utf8 (fs_utf8.h). mingw-w64's narrow
 * opendir()/readdir() read the path and the names in the active code page
 * and stop at MAX_PATH, so a mods folder outside the code page could not be
 * listed at all; the wide listing has no such limit.
 *
 * Path access used to differ the same way, and this file carried its own
 * `#if defined(_WIN32)` open/stat pair to route around it. It no longer needs
 * one: every byte and every existence question goes through mod_source, which
 * owns that boundary for both pack kinds at once. The listing is the only
 * filesystem call left in this file. */

/* mdkr_mod_manifest_parse() refuses input at or beyond its own 8 KiB bound
 * (MDKR_MOD_MANIFEST_TEXT_MAX), so reading further than that could only produce
 * a rejection with the bytes already in hand. This buffer is exactly that
 * bound, which leaves both over-length cases landing on the same number: a
 * pack.ini of exactly 8192 bytes fits here and is refused by the parser's own
 * check, and a longer one does not fit and comes back as
 * MDKR_MOD_SOURCE_BUFFER_TOO_SMALL. One bound, stated once, rather than a
 * second cap here that could drift away from it. */
#define MDKR_MOD_REGISTRY_MANIFEST_READ_MAX 8192

/* ---------------------------------------------------------------- paths */

/* Locale-independent on purpose: load order is part of the contract a pack
 * author relies on, and it must not change with the player's locale. */
static int ascii_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (int)(c + ('a' - 'A')) : (int)c;
}

static int ascii_casecmp(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        int lower_a = ascii_lower((unsigned char)*a);
        int lower_b = ascii_lower((unsigned char)*b);
        if (lower_a != lower_b) return lower_a < lower_b ? -1 : 1;
        a++;
        b++;
    }
    if (*a == *b) return 0;
    return *a == '\0' ? -1 : 1;
}

static void copy_bounded(char *dest, size_t dest_size, const char *source) {
    size_t length;

    if (dest == NULL || dest_size == 0) return;
    if (source == NULL) {
        dest[0] = '\0';
        return;
    }
    length = strlen(source);
    if (length >= dest_size) length = dest_size - 1;
    memcpy(dest, source, length);
    dest[length] = '\0';
}

/* There is no path validator in this file. It had one — a private
 * `path_is_safe_relative()` whose own comment said "a second copy is how one
 * caller gets fixed and the other keeps the hole" — and then the zip arm
 * arrived with a second copy in mod_source.c, which is precisely what that
 * comment predicted. The two even disagreed: this one treated '\' as an
 * ordinary separator and only checked for a drive letter at offset 1, while
 * mod_source's refuses '\' and ':' outright.
 *
 * mdkr_mod_source_path_is_safe() is now the only one, and it is stricter, so
 * every name this module used to accept and mod_source would have refused is
 * now refused at discovery with a reason rather than accepted here and
 * rejected one layer down.
 *
 * Joins with '/', which every platform here accepts. Returns 0 rather than
 * truncating: a truncated path names a different file, and silently probing a
 * different file is worse than not finding one. */
static int path_join(char *out, size_t out_size, const char *base,
                     const char *leaf) {
    size_t base_length;
    size_t leaf_length;

    if (out == NULL || out_size == 0) return 0;
    out[0] = '\0';
    if (base == NULL || leaf == NULL) return 0;

    base_length = strlen(base);
    while (base_length > 0 &&
           (base[base_length - 1] == '/' || base[base_length - 1] == '\\')) {
        base_length--;
    }
    leaf_length = strlen(leaf);
    if (base_length + 1 + leaf_length + 1 > out_size) return 0;

    memcpy(out, base, base_length);
    out[base_length] = '/';
    memcpy(out + base_length + 1, leaf, leaf_length);
    out[base_length + 1 + leaf_length] = '\0';
    return 1;
}

static const char *directory_name_of(const char *path) {
    const char *last = path;
    const char *cursor;

    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') last = cursor + 1;
    }
    return last;
}

/* ---------------------------------------------------------- pack shapes */

/* 1 when `name` ends in a case-insensitive ".zip" and has something in front of
 * it. Only used to decide whether a thing that is not a directory is worth
 * offering to the archive reader; the archive itself still has to parse, so a
 * file that is merely named ".zip" fails at open, not here. */
static int name_is_zip(const char *name) {
    size_t length = strlen(name);

    if (length < 5) return 0; /* at least one character before the suffix */
    return name[length - 4] == '.' &&
           ascii_lower((unsigned char)name[length - 3]) == 'z' &&
           ascii_lower((unsigned char)name[length - 2]) == 'i' &&
           ascii_lower((unsigned char)name[length - 1]) == 'p';
}

/* Opens `root` as whichever kind it turns out to be, and reports which through
 * `*out_is_zip`. NULL when it is neither an openable directory nor a readable
 * archive.
 *
 * The directory attempt comes first and is what classifies: mod_source's
 * directory arm refuses anything that is not a directory, so this needs no stat
 * of its own and cannot disagree with the reader about what a directory is. */
static MdkrModSource *open_pack_root(const char *root, const char *name,
                                     int *out_is_zip) {
    MdkrModSource *source = mdkr_mod_source_open(root, 0);

    *out_is_zip = 0;
    if (source != NULL) return source;
    if (!name_is_zip(name)) return NULL;

    source = mdkr_mod_source_open(root, 1);
    if (source == NULL) return NULL;
    *out_is_zip = 1;
    return source;
}

/* ------------------------------------------------------------ name index */

/* Every name every pack serves, indexed once at scan (issue #66).
 *
 * The texture store asks whether an enabled pack holds textures/<digest>.png
 * for every new texture the game draws, and almost every answer is no. Asking
 * the pack itself opened it -- for a zip, a parse of its whole central
 * directory, about 1.5 ms for a 2,120-entry pack -- once per question, inside
 * the frame. The answer only changes when the packs on disk change, which is
 * exactly when the registry is rescanned, so it is computed here once and
 * answered from memory afterwards.
 *
 * One open-addressed table over one string pool, keyed by the exact name.
 * Each slot carries a bit per pack rather than one slot per (name, pack), so
 * a name several packs override costs one entry and one probe. */

#define MDKR_MOD_NAME_TABLE_MIN 1024u
#define MDKR_MOD_NAME_POOL_MIN  ((size_t)64u * 1024u)

static uint32_t name_hash(const char *name) {
    uint32_t hash = 2166136261u;   /* FNV-1a */
    while (*name != '\0') {
        hash ^= (uint32_t)(unsigned char)*name++;
        hash *= 16777619u;
    }
    return hash;
}

#define MDKR_MOD_DIGEST_NAME_CHARS (9u + 32u + 4u)   /* textures/ + 32 + .png */

static int ascii_ncasecmp(const char *a, const char *b, size_t count);

/* textures/<32 hex digits>.png in any case: the only shape a digest lookup can
 * match. Writes the lowercase form (the index key) when `folded` is given.
 *
 * Any case, because 1.7.0 found these files by asking the filesystem, and the
 * filesystems macOS and Windows ship ignore case: a pack spelling them
 * Textures/ABC...PNG worked there and must keep working (#66). Only this shape
 * is folded. Every other name is matched exactly, as it always was. */
static int digest_name_fold(const char *rel, char *folded) {
    size_t index;

    if (strlen(rel) != MDKR_MOD_DIGEST_NAME_CHARS) return 0;
    if (ascii_ncasecmp(rel, "textures/", 9) != 0) return 0;
    if (ascii_ncasecmp(rel + 41, ".png", 4) != 0) return 0;
    for (index = 9; index < 41; index++) {
        if (!isxdigit((unsigned char)rel[index])) return 0;
    }
    if (folded != NULL) {
        for (index = 0; index < MDKR_MOD_DIGEST_NAME_CHARS; index++) {
            folded[index] = (char)ascii_lower((unsigned char)rel[index]);
        }
        folded[MDKR_MOD_DIGEST_NAME_CHARS] = '\0';
    }
    return 1;
}

static int is_digest_texture_name(const char *rel) {
    return digest_name_fold(rel, NULL);
}

/* A file under textures/ (any case) that is NOT digest-shaped -- another name,
 * another image format, a subfolder: a file a digest pack ships that no
 * lookup can ever reach. Hidden names (any component starting with a dot) are
 * left out: those are what editors and archivers leave behind, not the
 * author's, and the folder walk never lists them anyway. */
static int is_unusable_texture_name(const char *rel) {
    const char *component;
    if (strlen(rel) <= 9) return 0;
    if (ascii_ncasecmp(rel, "textures/", 9) != 0) return 0;
    for (component = rel + 9; component != NULL;
         component = strchr(component, '/')) {
        if (*component == '/') component++;
        if (*component == '.') return 0;
    }
    return !is_digest_texture_name(rel);
}

/* The key a name is indexed and looked up under: its lowercase form when it
 * is a texture digest, itself otherwise. */
static const char *name_key(const char *rel, char *buffer) {
    return digest_name_fold(rel, buffer) ? buffer : rel;
}

static int name_table_grow(MdkrModRegistry *reg, size_t capacity) {
    MdkrModNameSlot *slots =
        (MdkrModNameSlot *)calloc(capacity, sizeof *slots);
    size_t index;

    if (slots == NULL) return 0;
    for (index = 0; index < reg->name_capacity; index++) {
        const MdkrModNameSlot *old = &reg->names[index];
        size_t place;
        if (old->packs == 0) continue;
        place = (size_t)old->hash & (capacity - 1u);
        while (slots[place].packs != 0) place = (place + 1u) & (capacity - 1u);
        slots[place] = *old;
    }
    free(reg->names);
    reg->names = slots;
    reg->name_capacity = capacity;
    return 1;
}

/* Appends `text` to the pool; its offset, or UINT32_MAX when out of room. */
static uint32_t name_pool_put(MdkrModRegistry *reg, const char *text) {
    const size_t length = strlen(text) + 1u;
    uint32_t     offset;

    if (reg->name_pool_size + length > (size_t)UINT32_MAX - 1u) {
        return UINT32_MAX;
    }
    if (reg->name_pool_size + length > reg->name_pool_capacity) {
        size_t capacity = reg->name_pool_capacity == 0
                              ? MDKR_MOD_NAME_POOL_MIN
                              : reg->name_pool_capacity;
        char *grown;
        while (capacity < reg->name_pool_size + length) capacity *= 2u;
        grown = (char *)realloc(reg->name_pool, capacity);
        if (grown == NULL) return UINT32_MAX;
        reg->name_pool = grown;
        reg->name_pool_capacity = capacity;
    }
    offset = (uint32_t)reg->name_pool_size;
    memcpy(reg->name_pool + reg->name_pool_size, text, length);
    reg->name_pool_size += length;
    return offset;
}

/* Which of two spellings of one digest name a pack holding both is read by:
 * 1 for `a`, 0 for `b`. The exact lowercase form first -- the name 1.7.0
 * asked for, and the one every published pack uses -- then plain byte order,
 * so the answer never depends on the order the scan met them in or on where
 * a table grow happened to put their slots (T12d). */
static int spelling_preferred(const char *key, const char *a, const char *b) {
    const int a_exact = strcmp(a, key) == 0;
    const int b_exact = strcmp(b, key) == 0;
    if (a_exact != b_exact) return a_exact;
    return strcmp(a, b) < 0;
}

/* Says, once per duplicate set aside, that a pack holds one digest name under
 * two spellings and which one is used. A folder pack can do this only on a
 * file system that keeps case apart; a zip can anywhere. */
static void note_case_duplicate(MdkrModRegistry *reg, int pack,
                                const char *used, const char *ignored) {
    reg->entries[pack].textures_case_duplicates++;
    fprintf(stderr,
            "[MODS] %s: %s and %s differ only in case; using %s\n",
            reg->entries[pack].manifest.name, used, ignored, used);
}

/* The pool offset for `name` as a slot's spelling: the key's own text when
 * the two are the same, so an ordinary name is stored once. */
static uint32_t name_spelling_put(MdkrModRegistry *reg, const char *key,
                                  uint32_t key_offset, const char *name) {
    return strcmp(key, name) == 0 ? key_offset : name_pool_put(reg, name);
}

/* Records that pack `pack` serves `name`. 0 when memory ran out, which the
 * caller turns into "this pack's index is incomplete", never into a wrong
 * "absent".
 *
 * One slot per (key, spelling): packs that spell a name the same way share a
 * slot and its pack bits, and a digest name two packs spell differently gets
 * two slots under one key, each knowing how to read it back.
 *
 * A pack's bit is in at most ONE slot per key. A pack that holds a digest
 * name twice, differing only in case, is listed under the spelling
 * spelling_preferred() picks and the other is set aside with a note. That is
 * also what bounds name_hits(): at most one hit per pack. */
static int name_index_add(MdkrModRegistry *reg, const char *name, int pack) {
    char             folded[MDKR_MOD_DIGEST_NAME_CHARS + 1];
    const char      *key = name_key(name, folded);
    const uint32_t   hash = name_hash(key);
    const uint64_t   bit = UINT64_C(1) << pack;
    MdkrModNameSlot *same = NULL;   /* this key, spelled exactly as `name` */
    MdkrModNameSlot *held = NULL;   /* this key, already holding `pack` */
    size_t           mask;
    size_t           index;
    uint32_t         key_offset;
    uint32_t         spelling_offset;

    if (reg->name_capacity == 0 ||
        (reg->name_count + 1u) * 10u >= reg->name_capacity * 7u) {
        size_t capacity = reg->name_capacity == 0 ? MDKR_MOD_NAME_TABLE_MIN
                                                  : reg->name_capacity * 2u;
        if (!name_table_grow(reg, capacity)) return 0;
    }
    /* The whole run, not just up to the first match: the slot already holding
     * this pack under another spelling may lie after the exact match. */
    mask = reg->name_capacity - 1u;
    index = (size_t)hash & mask;
    while (reg->names[index].packs != 0) {
        MdkrModNameSlot *slot = &reg->names[index];
        if (slot->hash == hash &&
            strcmp(reg->name_pool + slot->offset, key) == 0) {
            if (same == NULL &&
                strcmp(reg->name_pool + slot->spelling, name) == 0) {
                same = slot;
            }
            if ((slot->packs & bit) != 0) held = slot;
        }
        index = (index + 1u) & mask;
    }

    if (held != NULL && held == same) return 1;   /* listed twice; one entry */
    if (held != NULL) {
        const char *current = reg->name_pool + held->spelling;
        if (spelling_preferred(key, current, name)) {
            note_case_duplicate(reg, pack, current, name);
            return 1;
        }
        note_case_duplicate(reg, pack, name, current);
        if (held->packs == bit) {
            /* This pack's slot alone: respell it in place. Its run position
             * is unchanged, and a slot is never emptied (that would cut the
             * probe run). If another slot already spells it this way, the
             * two now share a spelling with disjoint packs, which name_hits()
             * reports as two hits, each correct. Out of memory leaves the
             * old spelling, which still reads a real file, and says the
             * pack's index is incomplete. */
            spelling_offset = name_spelling_put(reg, key, held->offset, name);
            if (spelling_offset == UINT32_MAX) return 0;
            held->spelling = spelling_offset;
            return 1;
        }
        held->packs &= ~bit;    /* others still read it that way */
    }
    if (same != NULL) {
        same->packs |= bit;
        return 1;
    }

    key_offset = name_pool_put(reg, key);
    if (key_offset == UINT32_MAX) return 0;
    spelling_offset = name_spelling_put(reg, key, key_offset, name);
    if (spelling_offset == UINT32_MAX) return 0;
    reg->names[index].packs = bit;
    reg->names[index].hash = hash;
    reg->names[index].offset = key_offset;
    reg->names[index].spelling = spelling_offset;
    reg->name_count++;
    return 1;
}

/* Calls `visit` for every slot indexed under `relative_path`'s key, stopping
 * early when it returns non-zero. Slots are never removed, so every slot of a
 * key lies on its probe run before the first empty one. */
typedef int (*NameSlotVisit)(const MdkrModRegistry *reg,
                             const MdkrModNameSlot *slot, void *user);

static void name_slots_for(const MdkrModRegistry *reg,
                           const char *relative_path, NameSlotVisit visit,
                           void *user) {
    char        folded[MDKR_MOD_DIGEST_NAME_CHARS + 1];
    const char *key;
    uint32_t    hash;
    size_t      mask;
    size_t      index;
    size_t      probe;

    if (reg == NULL || reg->name_capacity == 0) return;
    key = name_key(relative_path, folded);
    hash = name_hash(key);
    mask = reg->name_capacity - 1u;
    index = (size_t)hash & mask;
    for (probe = 0; probe < reg->name_capacity; probe++) {
        const MdkrModNameSlot *slot = &reg->names[index];
        if (slot->packs == 0) return;
        if (slot->hash == hash &&
            strcmp(reg->name_pool + slot->offset, key) == 0 &&
            visit(reg, slot, user) != 0) {
            return;
        }
        index = (index + 1u) & mask;
    }
}

static void name_index_release(MdkrModRegistry *reg) {
    if (reg == NULL) return;
    free(reg->names);
    free(reg->name_pool);
    reg->names = NULL;
    reg->name_capacity = 0;
    reg->name_count = 0;
    reg->name_pool = NULL;
    reg->name_pool_size = 0;
    reg->name_pool_capacity = 0;
    reg->names_unindexed = 0;
}

/* --------------------------------------------------------------- skipping */

/* Never drops a rejection. Once the table is full the final slot is rewritten
 * to say so, so the count stays inside the array while the player is still
 * told that more went wrong than is listed.
 *
 * Both halves of that slot are rewritten, not just the reason. Settings ->
 * Content renders the list as "<name> — <reason>", so overwriting the reason
 * alone leaves the row reading one pack's name against a different pack's
 * explanation: the screen that exists to tell a player why a pack did not load
 * would be lying, in the one case where the most has already gone wrong.
 *
 * "More packs" as the name, chosen so the row reads as a summary of the rest
 * rather than as a pack: it is plural, so no single thing the player installed
 * can be mistaken for it, and it makes a sentence with the reason next to it
 * in the one place this text is ever seen. A pack directory could in principle
 * be named that too, but then the row still says something true about the
 * overflow, which a stale name does not. */

/* ------------------------------------------------------- Rice/GLideN64 */

/* Parse `<rom name>#<8 hex>#<fmt>#<siz>_<variant>.png` out of one relative
 * path. The leading directories and the ROM-name field are ignored on purpose:
 * every pack in the wild nests its files differently, and the ROM name is the
 * author's spelling of a game we already know we are.
 *
 * Returns 1 and fills the outputs only for a name that is entirely this shape.
 * Anything else -- a readme, a stray export, the author's own screenshot -- is
 * simply not a texture, and is skipped without comment. */
/* Release the Rice index. Called by BOTH init and shutdown: init memsets the
 * whole registry, so without this a second scan -- the launcher's settings
 * panel builds one, the engine builds another -- drops the first index's array
 * and every path string in it on the floor. */
static void rice_index_release(MdkrModRegistry *reg) {
    int i;
    if (reg == NULL || reg->rice == NULL) return;
    for (i = 0; i < reg->rice_count; ++i) {
        free(reg->rice[i].all);
        free(reg->rice[i].rgb);
        free(reg->rice[i].alpha);
    }
    free(reg->rice);
    reg->rice = NULL;
    reg->rice_count = 0;
    reg->rice_capacity = 0;
}

/* Bounded ASCII case-insensitive compare. The registry already has
 * ascii_casecmp for whole strings; this is the fixed-length form the variant
 * suffixes need, and neither one touches the C locale. */
static int ascii_ncasecmp(const char *a, const char *b, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return (int)ca - (int)cb;
    }
    return 0;
}

/* True when `count` characters starting at `text` are all hex digits. */
static int is_hex_run(const char *text, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (!isxdigit((unsigned char)text[i])) return 0;
    }
    return 1;
}

static int rice_parse_name(const char *rel, uint32_t *out_crc, int *out_fmt,
                           int *out_siz, int *out_variant) {
    const char *base = rel;
    const char *dot;
    const char *cursor;
    const char *p;

    for (p = rel; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    dot = strrchr(base, '.');
    if (dot == NULL || ascii_casecmp(dot, ".png") != 0) return 0;

    /* Walk the fields FORWARD from the texture CRC rather than counting '#'
     * from the end. Both shapes the format uses are then the same parse:
     *
     *   <rom>#<crc>#<fmt>#<siz>_<variant>.png
     *   <rom>#<crc>#<fmt>#<siz>#<palette>_<variant>.png   (colour-index)
     *
     * A ROM name may itself contain '#', so the CRC is found as the first
     * eight-hex-digit field that is followed by the rest of a well-formed
     * identity -- not by position.
     *
     * The palette CRC is parsed and DISCARDED, and that is a real limitation
     * rather than a simplification. A colour-index texture's palette hash
     * distinguishes two pictures that share pixel indices but not colours; key
     * on it and they stay apart. We cannot, because mdkr_rice_crc32() hashes
     * the texel span and nothing else -- the TLUT is never hashed, so a
     * palette-keyed name has nothing to match against at draw time. Keeping the
     * field in the key would make those entries permanently unmatchable, which
     * is strictly worse than collapsing them. So they collapse: where a pack
     * supplies several palette variants of one texture, the last one it lists
     * wins for all of them.
     *
     * DKR-R (ThatGuyMcd/DKR-R, runtime-recomp/src/game/rice_texture_pack_policy.hpp)
     * keys on the palette and can, because its RT64 patch computes the palette
     * hash alongside the texel hash. Hashing the TLUT here is the work that
     * would close this; it is not needed by any pack measured so far -- the
     * V0.1.6 pack carries a palette field on none of its 1663 identities. */
    for (cursor = base; cursor < dot; ++cursor) {
        const char *field;
        const char *end_of_field;
        char number[8];
        size_t length;
        unsigned long value;
        char hex[9];
        int fmt_value = 0;
        int siz_value = 0;

        if (*cursor != '#') continue;
        if (dot - cursor < 10) break;              /* no room for #CRC#f#s */
        if (!is_hex_run(cursor + 1, 8)) continue;
        if (cursor[9] != '#') continue;

        memcpy(hex, cursor + 1, 8);
        hex[8] = '\0';
        value = strtoul(hex, NULL, 16);

        /* Format: digits up to the next '#'. */
        field = cursor + 10;
        for (end_of_field = field; end_of_field < dot && *end_of_field != '#';
             ++end_of_field) {
        }
        length = (size_t)(end_of_field - field);
        if (end_of_field >= dot || length == 0 || length >= sizeof number) {
            continue;
        }
        memcpy(number, field, length);
        number[length] = '\0';
        for (p = number; *p != '\0'; ++p) {
            if (!isdigit((unsigned char)*p)) break;
        }
        if (*p != '\0') continue;
        fmt_value = atoi(number);

        /* Size: digits up to '#' (a palette follows) or '_' (it does not). */
        field = end_of_field + 1;
        for (end_of_field = field; end_of_field < dot && *end_of_field != '#' &&
                                   *end_of_field != '_'; ++end_of_field) {
        }
        length = (size_t)(end_of_field - field);
        if (end_of_field >= dot || length == 0 || length >= sizeof number) {
            continue;
        }
        memcpy(number, field, length);
        number[length] = '\0';
        for (p = number; *p != '\0'; ++p) {
            if (!isdigit((unsigned char)*p)) break;
        }
        if (*p != '\0') continue;
        siz_value = atoi(number);

        /* Skip an optional palette field, then take the variant suffix. */
        if (*end_of_field == '#') {
            for (++end_of_field; end_of_field < dot && *end_of_field != '_';
                 ++end_of_field) {
            }
            if (end_of_field >= dot) continue;
        }
        if (*end_of_field != '_') continue;
        {
            /* Case-insensitive, like the `.png` test above. Rice packs are two
             * decades of assets from many authors and many tools, and a pack
             * that shouts _ALL is the same pack. Refusing it would look to the
             * player exactly like a pack that simply did not work. */
            size_t suffix = (size_t)(dot - end_of_field);
            if (suffix == 4 && ascii_ncasecmp(end_of_field, "_all", 4) == 0) {
                *out_variant = 0;
            } else if (suffix == 4 &&
                       ascii_ncasecmp(end_of_field, "_rgb", 4) == 0) {
                *out_variant = 1;
            } else if (suffix == 2 &&
                       ascii_ncasecmp(end_of_field, "_a", 2) == 0) {
                *out_variant = 2;
            } else {
                continue;
            }
        }
        *out_crc = (uint32_t)value;
        *out_fmt = fmt_value;
        *out_siz = siz_value;
        return 1;
    }
    return 0;
}

static MdkrModRiceTexture *rice_slot(MdkrModRegistry *reg, uint32_t crc,
                                     int fmt, int siz, int pack) {
    int i;
    /* Matched on the PACK too. Deduping across packs let whichever pack was
     * scanned first own an identity permanently: a higher-priority pack could
     * never override it, and disabling the owner returned "no texture" instead
     * of falling through to another pack that also supplies it. */
    for (i = 0; i < reg->rice_count; ++i) {
        MdkrModRiceTexture *found = &reg->rice[i];
        if (found->crc == crc && found->fmt == fmt && found->siz == siz &&
            found->pack == pack) {
            return found;
        }
    }
    if (reg->rice_count == reg->rice_capacity) {
        int capacity = reg->rice_capacity == 0 ? 256 : reg->rice_capacity * 2;
        MdkrModRiceTexture *grown =
            (MdkrModRiceTexture *)realloc(reg->rice,
                                          (size_t)capacity * sizeof *grown);
        if (grown == NULL) return NULL;
        reg->rice = grown;
        reg->rice_capacity = capacity;
    }
    {
        MdkrModRiceTexture *fresh = &reg->rice[reg->rice_count++];
        memset(fresh, 0, sizeof *fresh);
        fresh->crc = crc; fresh->fmt = fmt; fresh->siz = siz; fresh->pack = pack;
        return fresh;
    }
}

typedef struct RiceScan {
    MdkrModRegistry *reg;
    int   pack;
    int   textures;
    int   probe_only;
} RiceScan;

static int rice_visit(const char *rel, void *user) {
    RiceScan *scan = (RiceScan *)user;
    uint32_t crc = 0;
    int fmt = 0, siz = 0, variant = 0;
    MdkrModRiceTexture *slot;
    char **field;
    size_t length;

    if (!rice_parse_name(rel, &crc, &fmt, &siz, &variant)) return 0;
    ++scan->textures;
    /* The probe stops at the first texture: deciding whether a manifest-less
     * folder is a Rice pack does not require reading two thousand names. */
    if (scan->probe_only) return 1;

    slot = rice_slot(scan->reg, crc, fmt, siz, scan->pack);
    if (slot == NULL) return 1;   /* out of memory: stop, keep what we have */
    field = variant == 0 ? &slot->all : (variant == 1 ? &slot->rgb : &slot->alpha);
    if (*field != NULL) return 0;  /* first one wins; packs do duplicate */
    length = strlen(rel) + 1;
    *field = (char *)malloc(length);
    if (*field != NULL) memcpy(*field, rel, length);
    return 0;
}

/* The post-sort walk of one pack: every name into the name index, and for a
 * Rice pack every texture-shaped name into the Rice index as well. One walk
 * feeds both, so a Rice pack is enumerated once at scan rather than twice. */
typedef struct PackScan {
    MdkrModRegistry *reg;
    int      pack;
    int      rice;
    int      failed;
    RiceScan rice_scan;
} PackScan;

static int pack_index_visit(const char *rel, void *user) {
    PackScan *scan = (PackScan *)user;

    if (!name_index_add(scan->reg, rel, scan->pack)) {
        scan->failed = 1;
        return 1;
    }
    if (is_digest_texture_name(rel)) {
        scan->reg->entries[scan->pack].digest_textures++;
    } else if (!scan->rice && is_unusable_texture_name(rel)) {
        MdkrModEntry *entry = &scan->reg->entries[scan->pack];
        if (entry->textures_skipped++ == 0) {
            copy_bounded(entry->textures_skipped_first,
                         sizeof entry->textures_skipped_first, rel);
        }
    }
    if (scan->rice && rice_visit(rel, &scan->rice_scan) != 0) {
        /* Out of memory for the Rice index: stop, keep what we have, and
         * say this pack's names are not all known. */
        scan->failed = 1;
        return 1;
    }
    return 0;
}

static void registry_add_skip(MdkrModRegistry *reg, const char *name,
                              const char *reason) {
    if (reg->skipped >= MDKR_MOD_MAX_PACKS) {
        copy_bounded(reg->skip_name[MDKR_MOD_MAX_PACKS - 1],
                     sizeof reg->skip_name[0], "More packs");
        copy_bounded(reg->skip_reason[MDKR_MOD_MAX_PACKS - 1],
                     sizeof reg->skip_reason[0],
                     "further packs were skipped; this list is full");
        return;
    }
    copy_bounded(reg->skip_name[reg->skipped], sizeof reg->skip_name[0], name);
    copy_bounded(reg->skip_reason[reg->skipped], sizeof reg->skip_reason[0],
                 reason);
    reg->skipped++;
}

/* --------------------------------------------------------------- ordering */

static int entry_order(const MdkrModEntry *left, const MdkrModEntry *right) {
    if (left->manifest.priority != right->manifest.priority) {
        return left->manifest.priority < right->manifest.priority ? -1 : 1;
    }
    return ascii_casecmp(directory_name_of(left->root),
                         directory_name_of(right->root));
}

/* Insertion sort, deliberately, not qsort: qsort is not required to be stable,
 * and equal-priority ordering is part of what a pack author is promised. */
static void registry_sort(MdkrModRegistry *reg) {
    int index;

    for (index = 1; index < reg->count; index++) {
        MdkrModEntry held = reg->entries[index];
        int scan = index - 1;
        while (scan >= 0 && entry_order(&reg->entries[scan], &held) > 0) {
            reg->entries[scan + 1] = reg->entries[scan];
            scan--;
        }
        reg->entries[scan + 1] = held;
    }
}

/* ------------------------------------------------------------------- API */

/* The walk that decides whether a manifest-less source is a Rice pack, held
 * to an entry budget and interruptible before every file and folder. */
typedef struct ProbeWalk {
    RiceScan rice;
    unsigned long long entries;
    unsigned long long max_entries;
    MdkrModProbeStop stop;
    void *stop_user;
    int over_budget;
    int stopped;
} ProbeWalk;

static int probe_walk_visit(const char *rel, int is_directory, void *user) {
    ProbeWalk *walk = (ProbeWalk *)user;
    if (walk->stop != NULL && walk->stop(walk->stop_user) != 0) {
        walk->stopped = 1;
        return 1;
    }
    if (walk->entries >= walk->max_entries) {
        walk->over_budget = 1;
        return 1;
    }
    walk->entries++;
    return is_directory ? 0 : rice_visit(rel, &walk->rice);
}

int mdkr_mod_registry_probe_source(MdkrModSource *source, const char *name,
    MdkrModManifest *manifest, int *is_rice, char *reason, size_t reason_size) {
    return mdkr_mod_registry_probe_source_bounded(
               source, name, ULLONG_MAX, NULL, NULL, manifest, is_rice,
               reason, reason_size) == MDKR_MOD_PROBE_ADMITTED;
}

int mdkr_mod_registry_probe_source_bounded(MdkrModSource *source, const char *name,
    unsigned long long max_entries, MdkrModProbeStop stop, void *stop_user,
    MdkrModManifest *manifest, int *is_rice, char *reason, size_t reason_size) {
    char text[MDKR_MOD_REGISTRY_MANIFEST_READ_MAX];
    size_t length = 0;
    int result;
    if (!manifest || !is_rice || !reason || !reason_size) return MDKR_MOD_PROBE_REFUSED;
    memset(manifest, 0, sizeof(*manifest));
    *is_rice = 0;
    reason[0] = '\0';
    if (!source || !name || !mdkr_mod_source_path_is_safe(name)) {
        copy_bounded(reason, reason_size, "the pack source or name is unavailable");
        return MDKR_MOD_PROBE_REFUSED;
    }
    if (name[0] == '.') {
        copy_bounded(reason, reason_size,
                     "its name starts with '.', which hides it from the game. "
                     "Rename it and try again");
        return MDKR_MOD_PROBE_REFUSED;
    }
    result = mdkr_mod_source_read(source, "pack.ini", text, sizeof text, &length);
    if (result == MDKR_MOD_SOURCE_ABSENT) {
        ProbeWalk walk;
        memset(&walk, 0, sizeof walk);
        walk.rice.probe_only = 1;
        walk.max_entries = max_entries;
        walk.stop = stop;
        walk.stop_user = stop_user;
        (void)mdkr_mod_source_enumerate_entries(source, probe_walk_visit, &walk);
        if (walk.stopped) {
            copy_bounded(reason, reason_size, "the check was stopped");
            return MDKR_MOD_PROBE_STOPPED;
        }
        if (!walk.rice.textures) {
            if (walk.over_budget) {
                copy_bounded(reason, reason_size,
                             "it has no pack.ini, and more files and folders "
                             "than the check may look through");
                return MDKR_MOD_PROBE_OVER_BUDGET;
            }
            copy_bounded(reason, reason_size, "this pack has no pack.ini");
            return MDKR_MOD_PROBE_REFUSED;
        }
        copy_bounded(manifest->name, sizeof manifest->name, name);
        manifest->priority = 100;
        manifest->enabled = 1;
        *is_rice = 1;
        return MDKR_MOD_PROBE_ADMITTED;
    }
    if (result == MDKR_MOD_SOURCE_BUFFER_TOO_SMALL || result == MDKR_MOD_SOURCE_TOO_LARGE) {
        snprintf(reason, reason_size, "its pack.ini is larger than %d bytes",
                 MDKR_MOD_REGISTRY_MANIFEST_READ_MAX);
        return MDKR_MOD_PROBE_REFUSED;
    }
    if (result != MDKR_MOD_SOURCE_OK) {
        copy_bounded(reason, reason_size, "this pack's pack.ini could not be read");
        return MDKR_MOD_PROBE_REFUSED;
    }
    return mdkr_mod_manifest_parse(text, length, manifest, reason, reason_size) == 0
        ? MDKR_MOD_PROBE_ADMITTED : MDKR_MOD_PROBE_REFUSED;
}

int mdkr_mod_registry_init(MdkrModRegistry *reg, const char *mods_dir) {
    MdkrDirectoryNames names;
    size_t name_index;

    if (reg == NULL) return -1;
    memset(reg, 0, sizeof(*reg));
    if (mods_dir == NULL || mods_dir[0] == '\0') return 0;

    if (mdkr_directory_names_utf8(mods_dir, &names) != 0) {
        const int listing_error = errno;
        int exists = 0;
        if (mdkr_path_query_utf8(mods_dir, &exists, NULL, NULL) == 0 &&
            !exists) {
            /* No mods/ at all is what almost every install looks like. */
            mdkr_directory_names_free(&names);
            return 0;
        }
        /* The folder is there but could not be listed completely. Keep the
         * packs read before the failure -- a shorter set, still sorted and
         * resolved by the same rules -- and say so, rather than look like an
         * install with no mods folder. The listing order is the file
         * system's, not priority order, so the packs left out can include
         * the one that would have won an asset: say that too. */
        reg->listing_incomplete = 1;
        fprintf(stderr,
                "[MODS] the mods folder %s could not be read completely (%s); "
                "using the %u entr%s read before that. Pack order may differ "
                "from a full read: a pack that was not read cannot supply "
                "its files, so a lower-priority pack may be used instead\n",
                mods_dir, strerror(listing_error), (unsigned)names.count,
                names.count == 1u ? "y" : "ies");
    }

    for (name_index = 0u; name_index < names.count; ++name_index) {
        const char *name = names.names[name_index];
        char pack_root[MDKR_MOD_PATH_MAX];
        char error[128];
        MdkrModManifest manifest;
        MdkrModSource *source;
        int is_zip = 0;
        int read_result;

        /* '.', '..', and the hidden bookkeeping directories that editors and
         * archivers leave behind. None of them are content, and none of them
         * are a problem worth telling the player about. */
        if (name[0] == '.') continue;

        /* The one validator, applied to a name the filesystem handed us before
         * that name is joined to anything. */
        if (!mdkr_mod_source_path_is_safe(name)) {
            registry_add_skip(reg, name,
                              "this pack's name cannot be used as a path");
            continue;
        }
        if (!path_join(pack_root, sizeof pack_root, mods_dir, name)) {
            registry_add_skip(reg, name,
                              "the path to this pack is too long to open");
            continue;
        }

        source = open_pack_root(pack_root, name, &is_zip);
        if (source == NULL) {
            /* Not a directory and not an archive we can read. A loose readme or
             * screenshot is not a failed pack and gets no complaint; something
             * named ".zip" that will not open is a pack the player meant to
             * install, so that one is reported. */
            if (name_is_zip(name)) {
                registry_add_skip(reg, name,
                                  "this archive could not be opened as a zip");
            }
            continue;
        }

        int is_rice = 0;
        read_result = mdkr_mod_registry_probe_source(source, name, &manifest,
                                                     &is_rice, error, sizeof error);
        mdkr_mod_source_close(source);
        if (!read_result) {
            registry_add_skip(reg, name, error);
            continue;
        }
        if (reg->count >= MDKR_MOD_MAX_PACKS) {
            char reason[128];
            snprintf(reason, sizeof reason,
                     "at most %d packs can be loaded; this one was not",
                     MDKR_MOD_MAX_PACKS);
            registry_add_skip(reg, name, reason);
            continue;
        }

        reg->entries[reg->count].manifest = manifest;
        /* path_join already proved this fits the identically sized field. */
        memcpy(reg->entries[reg->count].root, pack_root,
               strlen(pack_root) + 1);
        reg->entries[reg->count].is_zip = is_zip;
        reg->entries[reg->count].is_rice = is_rice;
        reg->count++;
    }
    /* A name with no UTF-8 spelling is never loaded (fs_utf8.h: every path
     * in the game is UTF-8). Say so, under a readable stand-in, rather than
     * leave the pack out without a word. */
    for (name_index = 0u; name_index < names.unnamed_count; ++name_index) {
        if (names.unnamed[name_index][0] == '.') continue;
        registry_add_skip(reg, names.unnamed[name_index],
                          "the name is not valid Unicode; rename it to load it");
    }
    mdkr_directory_names_free(&names);

    registry_sort(reg);

    /* Index LAST, because registry_sort() has just permuted entries[] by
     * priority and both indexes store entry indices. Capturing them during the
     * scan recorded pre-sort positions, so a Rice pack could end up consulting
     * a DIFFERENT pack's enabled flag -- silently, and dependent on whatever
     * order the listing happened to return.
     *
     * Ascending priority order also gives the format's own precedence for free:
     * a later pack's entry for the same identity is appended after an earlier
     * one, and the lookup takes the last enabled match, which is the same "the
     * last pack that owns the file wins" rule mdkr_mod_registry_open_file
     * applies to every other asset.
     *
     * Every pack is walked, not only Rice ones: the name index is what lets a
     * digest lookup answer without opening anything. The root is reopened with
     * the kind the scan already established -- open_pack_root() decides "is
     * this a zip" from the NAME, which for a manifest pack is its pack.ini
     * name, not its file name. */
    {
        int index;
        for (index = 0; index < reg->count; ++index) {
            PackScan scan;
            MdkrModSource *source_for_index;
            memset(&scan, 0, sizeof scan);
            scan.reg = reg;
            scan.pack = index;
            scan.rice = reg->entries[index].is_rice;
            scan.rice_scan.reg = reg;
            scan.rice_scan.pack = index;
            source_for_index = mdkr_mod_source_open(reg->entries[index].root,
                                                    reg->entries[index].is_zip);
            if (source_for_index == NULL) {
                reg->names_unindexed |= UINT64_C(1) << index;
                continue;
            }
            if (mdkr_mod_source_enumerate(source_for_index, pack_index_visit,
                                          &scan) == 0 &&
                !scan.failed) {
                reg->entries[index].names_indexed = 1;
            } else {
                reg->names_unindexed |= UINT64_C(1) << index;
            }
            mdkr_mod_source_close(source_for_index);
        }
    }
    return 0;
}

void mdkr_mod_registry_shutdown(MdkrModRegistry *reg) {
    /* Frees the two indexes, and makes a shut-down registry unusable rather
     * than stale — a resolve after shutdown finds no packs instead of pointing
     * at directories nobody rescanned. */
    if (reg == NULL) return;
    /* BEFORE the memset. Zeroing first leaves the loop below reading a count of
     * zero and a NULL array, so every path string leaks in silence. */
    rice_index_release(reg);
    name_index_release(reg);
    memset(reg, 0, sizeof(*reg));
}

const MdkrModRiceTexture *mdkr_mod_registry_rice_lookup(
    const MdkrModRegistry *reg, uint32_t crc, int fmt, int siz) {
    const MdkrModRiceTexture *best = NULL;
    int i;
    if (reg == NULL) return NULL;
    for (i = 0; i < reg->rice_count; ++i) {
        const MdkrModRiceTexture *found = &reg->rice[i];
        if (found->crc != crc || found->fmt != fmt || found->siz != siz) continue;
        if (found->pack < 0 || found->pack >= reg->count) continue;
        /* A pack the player switched off is still indexed -- they installed it
         * and should see it listed -- but it never wins a lookup, and another
         * pack supplying the same identity still can. */
        if (!reg->entries[found->pack].manifest.enabled) continue;
        /* Entries are appended in ascending priority order, so the last match
         * is the highest-priority pack that owns this identity. */
        best = found;
    }
    return best;
}

int mdkr_mod_registry_rice_count(const MdkrModRegistry *reg) {
    int i;
    int usable = 0;
    if (reg == NULL) return 0;
    /* Identities from ENABLED packs only. Two callers depend on that reading:
     * the texture store skips computing a Rice key at all when this is zero,
     * so a player who switched their pack off pays nothing per upload; and the
     * startup line stops announcing 1663 indexed identities for a pack it has
     * just reported as skipped. */
    for (i = 0; i < reg->rice_count; ++i) {
        const int pack = reg->rice[i].pack;
        if (pack < 0 || pack >= reg->count) continue;
        if (!reg->entries[pack].manifest.enabled) continue;
        ++usable;
    }
    return usable;
}

int mdkr_mod_registry_pack_rice_count(const MdkrModRegistry *reg, int index) {
    int i;
    int owned = 0;
    if (reg == NULL || index < 0 || index >= reg->count) return 0;
    for (i = 0; i < reg->rice_count; ++i) {
        if (reg->rice[i].pack == index) ++owned;
    }
    return owned;
}

int mdkr_mod_registry_count(const MdkrModRegistry *reg) {
    return reg != NULL ? reg->count : 0;
}

const MdkrModEntry *mdkr_mod_registry_entry(const MdkrModRegistry *reg, int i) {
    if (reg == NULL || i < 0 || i >= reg->count) return NULL;
    return &reg->entries[i];
}

int mdkr_mod_registry_skipped(const MdkrModRegistry *reg) {
    return reg != NULL ? reg->skipped : 0;
}

const char *mdkr_mod_registry_skip_reason(const MdkrModRegistry *reg, int i) {
    if (reg == NULL || i < 0 || i >= reg->skipped) return NULL;
    return reg->skip_reason[i];
}

int mdkr_mod_registry_open_file(const MdkrModRegistry *reg,
                                const char *relative_path,
                                MdkrModFile *out) {
    int index;

    if (out != NULL) {
        out->source = NULL;
        out->relative[0] = '\0';
        out->pack_index = -1;
    }
    if (reg == NULL || out == NULL) return 0;
    /* Before any pack is opened, so a rejected name is never even probed. The
     * arms below each check it again inside mod_source; that is the funnel
     * doing its job, not a redundancy to remove. */
    if (!mdkr_mod_source_path_is_safe(relative_path)) return 0;
    if (strlen(relative_path) + 1 > sizeof out->relative) return 0;

    /* Backwards: the list is ascending by priority, so the last pack that owns
     * the file is the one that wins. */
    for (index = reg->count - 1; index >= 0; index--) {
        const MdkrModEntry *candidate = &reg->entries[index];
        MdkrModSource *source;

        if (!candidate->manifest.enabled) continue;
        source = mdkr_mod_source_open(candidate->root, candidate->is_zip);
        if (source == NULL) continue; /* the pack went away since the scan */
        if (!mdkr_mod_source_has(source, relative_path)) {
            mdkr_mod_source_close(source);
            continue;
        }
        out->source = source;
        memcpy(out->relative, relative_path, strlen(relative_path) + 1);
        out->pack_index = index;
        return 1;
    }
    return 0;
}

void mdkr_mod_registry_close_file(MdkrModFile *file) {
    if (file == NULL) return;
    mdkr_mod_source_close(file->source);
    file->source = NULL;
    file->relative[0] = '\0';
    file->pack_index = -1;
}

int mdkr_mod_registry_resolve(const MdkrModRegistry *reg,
                              const char *relative_path,
                              char *out_path, size_t out_size) {
    int index;

    if (out_path != NULL && out_size > 0) out_path[0] = '\0';
    if (reg == NULL || out_path == NULL || out_size == 0) return 0;
    /* Before any filesystem call, so a rejected path is never even probed. */
    if (!mdkr_mod_source_path_is_safe(relative_path)) return 0;

    /* Backwards: the list is ascending by priority, so the last pack that owns
     * the file is the one that wins. */
    for (index = reg->count - 1; index >= 0; index--) {
        const MdkrModEntry *candidate = &reg->entries[index];
        char full_path[MDKR_MOD_PATH_MAX];
        MdkrModSource *source;
        size_t length;
        int present;

        if (!candidate->manifest.enabled) continue;
        /* An archive entry has no path to hand back; see the header. Content
         * lookups go through mdkr_mod_registry_open_file(), which does not
         * have this hole because it never speaks in paths at all. */
        if (candidate->is_zip) continue;
        if (!path_join(full_path, sizeof full_path, candidate->root,
                       relative_path)) {
            continue;
        }
        /* Existence is asked of mod_source rather than of stat, so "this pack
         * holds that name" means the same thing here as it does at every other
         * caller -- including the size ceiling, which a bare stat would miss. */
        source = mdkr_mod_source_open(candidate->root, 0);
        if (source == NULL) continue;
        present = mdkr_mod_source_has(source, relative_path);
        mdkr_mod_source_close(source);
        if (!present) continue;

        length = strlen(full_path);
        if (length + 1 > out_size) {
            out_path[0] = '\0';
            return 0;
        }
        memcpy(out_path, full_path, length + 1);
        return 1;
    }
    return 0;
}

static uint64_t registry_live_packs(const MdkrModRegistry *reg) {
    return reg->count >= 64 ? ~UINT64_C(0)
                            : (UINT64_C(1) << reg->count) - 1u;
}

static int name_packs_visit(const MdkrModRegistry *reg,
                            const MdkrModNameSlot *slot, void *user) {
    (void)reg;
    *(uint64_t *)user |= slot->packs;
    return 0;
}

uint64_t mdkr_mod_registry_name_packs(const MdkrModRegistry *reg,
                                      const char *relative_path) {
    uint64_t packs = 0;

    if (reg == NULL || reg->count <= 0) return 0;
    /* The same validator every pack read passes, so a name the packs could
     * never serve is never reported as served. */
    if (!mdkr_mod_source_path_is_safe(relative_path)) return 0;
    name_slots_for(reg, relative_path, name_packs_visit, &packs);
    return (packs | reg->names_unindexed) & registry_live_packs(reg);
}

/* Every hit of one key. name_index_add() keeps each pack in at most one slot
 * per key, so there are never more hits than packs. */
typedef struct NameHits {
    MdkrModNameHit all[MDKR_MOD_MAX_PACKS];
    int            count;
    uint64_t       live;
} NameHits;

static int name_hits_visit(const MdkrModRegistry *reg,
                           const MdkrModNameSlot *slot, void *user) {
    NameHits *hits = (NameHits *)user;
    if ((slot->packs & hits->live) == 0) return 0;
    /* Unreachable while the one-slot-per-pack rule holds; a stop, not an
     * overrun, if it ever does not. */
    if (hits->count >= MDKR_MOD_MAX_PACKS) return 1;
    hits->all[hits->count].packs = slot->packs & hits->live;
    hits->all[hits->count].name = reg->name_pool + slot->spelling;
    hits->count++;
    return 0;
}

/* The highest pack a hit serves: its rank, since entries[] ascends by
 * priority. */
static int name_hit_rank(const MdkrModNameHit *hit) {
    int pack = MDKR_MOD_MAX_PACKS - 1;
    while (pack > 0 && (hit->packs & (UINT64_C(1) << pack)) == 0) pack--;
    return pack;
}

int mdkr_mod_registry_name_hits(const MdkrModRegistry *reg,
                                const char *relative_path,
                                MdkrModNameHit *out, int max) {
    NameHits hits;
    int      index;

    if (reg == NULL || reg->count <= 0 || out == NULL || max <= 0) return 0;
    if (!mdkr_mod_source_path_is_safe(relative_path)) return 0;
    hits.count = 0;
    hits.live = registry_live_packs(reg);
    name_slots_for(reg, relative_path, name_hits_visit, &hits);
    /* Highest priority first, so a caller asking for fewer than every hit
     * keeps the ones that win rather than the ones indexed first (T12d: the
     * store asked for eight, and the ninth spelling -- the top pack's, which
     * is indexed last -- was dropped without a word). Insertion sort: there
     * are a handful at most. */
    for (index = 1; index < hits.count; index++) {
        const MdkrModNameHit moving = hits.all[index];
        const int            rank = name_hit_rank(&moving);
        int                  place = index;
        while (place > 0 && name_hit_rank(&hits.all[place - 1]) < rank) {
            hits.all[place] = hits.all[place - 1];
            place--;
        }
        hits.all[place] = moving;
    }
    if (hits.count > max) hits.count = max;
    for (index = 0; index < hits.count; index++) out[index] = hits.all[index];
    return hits.count;
}

int mdkr_mod_registry_find_file(const MdkrModRegistry *reg,
                                const char *relative_path) {
    const uint64_t packs = mdkr_mod_registry_name_packs(reg, relative_path);
    int index;

    if (packs == 0) return -1;
    /* Backwards, as open_file(): the last enabled holder wins. */
    for (index = reg->count - 1; index >= 0; index--) {
        if ((packs & (UINT64_C(1) << index)) == 0) continue;
        if (!reg->entries[index].manifest.enabled) continue;
        return index;
    }
    return -1;
}

