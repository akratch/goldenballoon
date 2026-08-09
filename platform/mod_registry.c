/**
 * mod_registry.c — see mod_registry.h.
 */
#include "mod_registry.h"
#include "mod_manifest.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
/* Windows builds are mingw-w64 only (MSVC is rejected outright in
 * CMakeLists.txt), and mingw-w64 ships <dirent.h>, so enumeration is the same
 * code on both platforms and there is no second Win32 walk to keep in step.
 * What does differ is path access: the narrow CRT reads paths in the active
 * code page and stops at MAX_PATH, both of which fs_utf8 already solves once
 * for the whole port. Everything here therefore opens and stats through
 * fs_utf8, and this file performs no UTF-8/UTF-16 conversion of its own. */
#include "fs_utf8.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* mdkr_mod_manifest_parse() refuses input at or beyond its own 8 KiB bound, so
 * reading further than that could only produce a rejection with the bytes
 * already in hand. Sizing the read buffer to exactly that bound means an
 * oversized pack.ini fills the buffer, fails the parser's length check, and
 * comes back with the parser's own wording — one bound, stated in one place,
 * rather than a second cap here that could drift away from it. */
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

/* THE path validator. Every relative path this module accepts from outside
 * itself — a pack directory name read from the filesystem, and the lookup path
 * a caller asks to resolve — passes through here before any filesystem call is
 * made with it. Keep it that way: a second copy is how one caller gets fixed
 * and the other keeps the hole.
 *
 * Accepts a non-empty relative path built from '/'- or '\'-separated
 * components. Rejects a leading separator, a drive prefix, an empty component
 * and any component that is exactly "..", which together cover every way a
 * pack could name something outside its own directory. A component such as
 * "..config" is a legal filename and is allowed; only the traversal component
 * itself is refused. Returns 1 when the path is safe to join. */
static int path_is_safe_relative(const char *relative_path) {
    size_t index = 0;
    size_t component_start = 0;

    if (relative_path == NULL || relative_path[0] == '\0') return 0;
    if (relative_path[0] == '/' || relative_path[0] == '\\') return 0;
    /* "C:\x" is absolute and "C:x" is drive-relative; neither stays put. */
    if (relative_path[1] == ':') return 0;

    for (;;) {
        char c = relative_path[index];
        if (c == '/' || c == '\\' || c == '\0') {
            size_t length = index - component_start;
            if (length == 0) return 0; /* "a//b", or a trailing separator */
            if (length == 2 && relative_path[component_start] == '.' &&
                relative_path[component_start + 1] == '.') {
                return 0;
            }
            if (c == '\0') break;
            component_start = index + 1;
        }
        index++;
    }
    return index < MDKR_MOD_PATH_MAX;
}

/* Joins with '/', which every platform here accepts. Returns 0 rather than
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

/* ------------------------------------------------------------ filesystem */

#if defined(_WIN32)
static FILE *registry_open_read(const char *path) {
    return mdkr_fopen_utf8(path, "rb");
}

static int path_is_directory(const char *path) {
    int is_directory = 0;
    return mdkr_path_query_utf8(path, NULL, NULL, &is_directory) == 0 &&
           is_directory;
}

static int path_is_regular_file(const char *path) {
    int is_regular = 0;
    return mdkr_path_query_utf8(path, NULL, &is_regular, NULL) == 0 &&
           is_regular;
}
#else
static FILE *registry_open_read(const char *path) {
    return fopen(path, "rb");
}

static int path_is_directory(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static int path_is_regular_file(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}
#endif

/* Returns the byte count read, or -1 when the file could not be read at all.
 * A file larger than `buffer_size` fills the buffer and is reported at that
 * length, which the manifest parser then refuses by its own bound. */
static long read_manifest_text(const char *path, char *buffer,
                               size_t buffer_size) {
    FILE *file = registry_open_read(path);
    size_t read_bytes;

    if (file == NULL) return -1;
    read_bytes = fread(buffer, 1, buffer_size, file);
    if (ferror(file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return (long)read_bytes;
}

/* --------------------------------------------------------------- skipping */

/* Never drops a rejection. Once the table is full the final slot is rewritten
 * to say so, so the count stays inside the array while the player is still
 * told that more went wrong than is listed. */
static void registry_add_skip(MdkrModRegistry *reg, const char *name,
                              const char *reason) {
    if (reg->skipped >= MDKR_MOD_MAX_PACKS) {
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
        char manifest_path[MDKR_MOD_PATH_MAX];
        char text[MDKR_MOD_REGISTRY_MANIFEST_READ_MAX];
        char error[128];
        MdkrModManifest manifest;
        long length;

        /* '.', '..', and the hidden bookkeeping directories that editors and
         * archivers leave behind. None of them are content, and none of them
         * are a problem worth telling the player about. */
        if (name[0] == '.') continue;

        if (!path_is_safe_relative(name)) {
            registry_add_skip(reg, name,
                              "the directory name cannot be used as a path");
            continue;
        }
        if (!path_join(pack_root, sizeof pack_root, mods_dir, name)) {
            registry_add_skip(reg, name,
                              "the path to this pack is too long to open");
            continue;
        }
        /* Loose files sitting in mods/ are not failed packs; zip packs arrive
         * in M3 and will be recognised here by extension. */
        if (!path_is_directory(pack_root)) continue;

        if (!path_join(manifest_path, sizeof manifest_path, pack_root,
                       "pack.ini")) {
            registry_add_skip(reg, name,
                              "the path to this pack is too long to open");
            continue;
        }
        length = read_manifest_text(manifest_path, text, sizeof text);
        if (length < 0) {
            registry_add_skip(reg, name,
                              "this directory has no readable pack.ini");
            continue;
        }
        if (mdkr_mod_manifest_parse(text, (size_t)length, &manifest, error,
                                    sizeof error) != 0) {
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
        reg->entries[reg->count].is_zip = 0;
        reg->count++;
    }
    closedir(directory);

    registry_sort(reg);
    return 0;
}

void mdkr_mod_registry_shutdown(MdkrModRegistry *reg) {
    /* Nothing is allocated, so this only exists to make a shut-down registry
     * unusable rather than stale — a resolve after shutdown finds no packs
     * instead of pointing at directories nobody rescanned. */
    if (reg == NULL) return;
    memset(reg, 0, sizeof(*reg));
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

int mdkr_mod_registry_resolve(const MdkrModRegistry *reg,
                              const char *relative_path,
                              char *out_path, size_t out_size) {
    int index;

    if (out_path != NULL && out_size > 0) out_path[0] = '\0';
    if (reg == NULL || out_path == NULL || out_size == 0) return 0;
    /* Before any filesystem call, so a rejected path is never even probed. */
    if (!path_is_safe_relative(relative_path)) return 0;

    /* Backwards: the list is ascending by priority, so the last pack that owns
     * the file is the one that wins. */
    for (index = reg->count - 1; index >= 0; index--) {
        const MdkrModEntry *candidate = &reg->entries[index];
        char full_path[MDKR_MOD_PATH_MAX];
        size_t length;

        if (!candidate->manifest.enabled) continue;
        if (candidate->is_zip) continue; /* archive lookup arrives in M3 */
        if (!path_join(full_path, sizeof full_path, candidate->root,
                       relative_path)) {
            continue;
        }
        if (!path_is_regular_file(full_path)) continue;

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
