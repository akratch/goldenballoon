/**
 * mod_registry.c — see mod_registry.h.
 */
#include "mod_registry.h"
#include "mod_manifest.h"
#include "mod_source.h"

#include <ctype.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

/* Windows builds are mingw-w64 only (MSVC is rejected outright in
 * CMakeLists.txt), and mingw-w64 ships <dirent.h>, so enumeration is the same
 * code on both platforms and there is no second Win32 walk to keep in step.
 *
 * What used to differ was path access — the narrow CRT reads paths in the
 * active code page and stops at MAX_PATH — and this file carried its own
 * `#if defined(_WIN32)` open/stat pair to route around it. It no longer needs
 * one: every byte and every existence question goes through mod_source, which
 * owns that boundary for both pack kinds at once. `readdir()` is the only
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

int mdkr_mod_registry_probe_source(MdkrModSource *source, const char *name,
    MdkrModManifest *manifest, int *is_rice, char *reason, size_t reason_size) {
    char text[MDKR_MOD_REGISTRY_MANIFEST_READ_MAX];
    size_t length = 0;
    int result;
    if (!manifest || !is_rice || !reason || !reason_size) return 0;
    memset(manifest, 0, sizeof(*manifest));
    *is_rice = 0;
    reason[0] = '\0';
    if (!source || !name || !mdkr_mod_source_path_is_safe(name)) {
        copy_bounded(reason, reason_size, "the pack source or name is unavailable");
        return 0;
    }
    result = mdkr_mod_source_read(source, "pack.ini", text, sizeof text, &length);
    if (result == MDKR_MOD_SOURCE_ABSENT) {
        RiceScan probe;
        memset(&probe, 0, sizeof probe);
        probe.probe_only = 1;
        (void)mdkr_mod_source_enumerate(source, rice_visit, &probe);
        if (!probe.textures) {
            copy_bounded(reason, reason_size, "this pack has no pack.ini");
            return 0;
        }
        copy_bounded(manifest->name, sizeof manifest->name, name);
        manifest->priority = 100;
        manifest->enabled = 1;
        *is_rice = 1;
        return 1;
    }
    if (result == MDKR_MOD_SOURCE_BUFFER_TOO_SMALL || result == MDKR_MOD_SOURCE_TOO_LARGE) {
        snprintf(reason, reason_size, "its pack.ini is larger than %d bytes",
                 MDKR_MOD_REGISTRY_MANIFEST_READ_MAX);
        return 0;
    }
    if (result != MDKR_MOD_SOURCE_OK) {
        copy_bounded(reason, reason_size, "this pack's pack.ini could not be read");
        return 0;
    }
    return mdkr_mod_manifest_parse(text, length, manifest, reason, reason_size) == 0;
}

int mdkr_mod_registry_init(MdkrModRegistry *reg, const char *mods_dir) {
    DIR *directory;
    struct dirent *entry;

    if (reg == NULL) return -1;
    memset(reg, 0, sizeof(*reg));
    if (mods_dir == NULL || mods_dir[0] == '\0') return 0;

    directory = opendir(mods_dir);
    if (directory == NULL) {
        /* No mods/ at all is what almost every install looks like. */
        return 0;
    }

    while ((entry = readdir(directory)) != NULL) {
        const char *name = entry->d_name;
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
    closedir(directory);

    registry_sort(reg);

    /* Index the Rice packs LAST, because registry_sort() has just permuted
     * entries[] by priority and the index stores entry indices. Capturing them
     * during the scan recorded pre-sort positions, so a Rice pack could end up
     * consulting a DIFFERENT pack's enabled flag -- silently, and dependent on
     * whatever order readdir happened to return.
     *
     * Ascending priority order also gives the format's own precedence for free:
     * a later pack's entry for the same identity is appended after an earlier
     * one, and the lookup takes the last enabled match, which is the same "the
     * last pack that owns the file wins" rule mdkr_mod_registry_open_file
     * applies to every other asset. */
    {
        int index;
        for (index = 0; index < reg->count; ++index) {
            RiceScan scan;
            MdkrModSource *source_for_index;
            int zip = reg->entries[index].is_zip;
            if (!reg->entries[index].is_rice) continue;
            memset(&scan, 0, sizeof scan);
            scan.reg = reg;
            scan.pack = index;
            source_for_index = open_pack_root(reg->entries[index].root,
                                              reg->entries[index].manifest.name,
                                              &zip);
            if (source_for_index == NULL) continue;
            mdkr_mod_source_enumerate(source_for_index, rice_visit, &scan);
            mdkr_mod_source_close(source_for_index);
        }
    }
    return 0;
}

void mdkr_mod_registry_shutdown(MdkrModRegistry *reg) {
    /* Nothing is allocated, so this only exists to make a shut-down registry
     * unusable rather than stale — a resolve after shutdown finds no packs
     * instead of pointing at directories nobody rescanned. */
    if (reg == NULL) return;
    /* BEFORE the memset. Zeroing first leaves the loop below reading a count of
     * zero and a NULL array, so every path string leaks in silence. */
    rice_index_release(reg);
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
