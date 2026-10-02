/*
 * test_mod_registry.c — discovery, ordering and resolution over a throwaway
 * pack tree.
 *
 * Every case builds its own mods directory beneath the scratch root given by
 * argv[1], so no test ever reads or writes a real user content directory and
 * no case can see another case's packs. The whole tree is removed before the
 * first case and after the last, so a rerun starts from nothing even if a
 * previous run aborted.
 */
#include "mod_registry.h"
#include "miniz.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#else
#include <fcntl.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0777)
#define TEST_RMDIR(path) rmdir(path)
#endif

static int failures;

static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); failures++; }
    else       { printf("ok   %s\n", what); }
}

static void fatal(const char *what, const char *detail) {
    printf("FATAL %s: %s\n", what, detail);
    exit(2);
}

/* snprintf with the return value checked, so the scratch-path helpers cannot
 * quietly build a truncated path and test the wrong file. */
static void path2(char *out, size_t size, const char *a, const char *b) {
    int written = snprintf(out, size, "%s/%s", a, b);
    if (written < 0 || (size_t)written >= size) fatal("scratch path too long", a);
}

static void path3(char *out, size_t size, const char *a, const char *b,
                  const char *c) {
    int written = snprintf(out, size, "%s/%s/%s", a, b, c);
    if (written < 0 || (size_t)written >= size) fatal("scratch path too long", a);
}

static int is_directory(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

/* Creates every ancestor directory of `path`. An existing directory is the
 * normal case and is not an error. */
static void make_parent_directories(const char *path) {
    char work[1024];
    size_t index;

    if (strlen(path) >= sizeof work) fatal("scratch path too long", path);
    memcpy(work, path, strlen(path) + 1);
    for (index = 1; work[index] != '\0'; index++) {
        if (work[index] != '/') continue;
        work[index] = '\0';
        if (TEST_MKDIR(work) != 0 && !is_directory(work)) {
            fatal("cannot create scratch directory", work);
        }
        work[index] = '/';
    }
}

static void write_text_file(const char *path, const char *text) {
    FILE *file;

    make_parent_directories(path);
    file = fopen(path, "wb");
    if (file == NULL) fatal("cannot create scratch file", path);
    if (text[0] != '\0' && fwrite(text, 1, strlen(text), file) != strlen(text)) {
        fclose(file);
        fatal("cannot write scratch file", path);
    }
    fclose(file);
}

/* Writes `text` to `root/pack/rel`, creating `root`, the pack directory and
 * any directory `rel` nests inside. */
static void write_pack_file(const char *root, const char *pack,
                            const char *rel, const char *text) {
    char full[1024];
    path3(full, sizeof full, root, pack, rel);
    write_text_file(full, text);
}

/* Only ever pointed at the scratch root. */
static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    struct dirent *entry;

    if (dir == NULL) {
        remove(path);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char child[1024];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        path2(child, sizeof child, path, entry->d_name);
        if (is_directory(child)) remove_tree(child);
        else                     remove(child);
    }
    closedir(dir);
    TEST_RMDIR(path);
}

/* ------------------------------------------------------------------ */

/* 1. Priority orders packs, and the highest priority wins a collision. */
static void test_priority_orders_packs(const char *scratch) {
    char mods[1024];
    MdkrModRegistry reg;
    char resolved[MDKR_MOD_PATH_MAX];

    path2(mods, sizeof mods, scratch, "priority");
    write_pack_file(mods, "alpha", "pack.ini", "[pack]\nname=Alpha\npriority=10\n");
    write_pack_file(mods, "bravo", "pack.ini", "[pack]\nname=Bravo\npriority=90\n");
    write_pack_file(mods, "alpha", "textures/deadbeef.png", "A");
    write_pack_file(mods, "bravo", "textures/deadbeef.png", "B");

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "registry initialises");
    expect(mdkr_mod_registry_count(&reg) == 2, "both packs discovered");
    expect(mdkr_mod_registry_entry(&reg, 0) != NULL &&
           mdkr_mod_registry_entry(&reg, 0)->manifest.priority == 10,
           "the list is ordered ascending by priority");
    expect(mdkr_mod_registry_resolve(&reg, "textures/deadbeef.png",
                                     resolved, sizeof resolved) == 1,
           "collision resolves");
    expect(strstr(resolved, "bravo") != NULL,
           "higher priority wins the collision");
    mdkr_mod_registry_shutdown(&reg);
}

/* 2. A malformed manifest disables one pack and no other. */
static void test_malformed_manifest_isolates(const char *scratch) {
    char mods[1024];
    MdkrModRegistry reg;

    path2(mods, sizeof mods, scratch, "malformed");
    write_pack_file(mods, "good", "pack.ini", "[pack]\nname=Good\n");
    write_pack_file(mods, "broken", "pack.ini", "[pack]\nauthor=NoName\n");

    expect(mdkr_mod_registry_init(&reg, mods) == 0,
           "a broken pack does not fail the registry");
    expect(mdkr_mod_registry_count(&reg) == 1, "only the good pack loads");
    expect(mdkr_mod_registry_skipped(&reg) == 1, "the broken pack is counted");
    expect(mdkr_mod_registry_skip_reason(&reg, 0) != NULL &&
           mdkr_mod_registry_skip_reason(&reg, 0)[0] != '\0',
           "the skip carries a reason");
    expect(strstr(mdkr_mod_registry_skip_reason(&reg, 0), "name") != NULL,
           "the reason names the key that was missing");
    mdkr_mod_registry_shutdown(&reg);
}

/* 3. No mods/ directory is the ordinary install, not a failure. */
static void test_missing_mods_dir_is_not_an_error(void) {
    MdkrModRegistry reg;

    expect(mdkr_mod_registry_init(&reg, "definitely/not/here") == 0,
           "absent mods dir is the normal case, not a failure");
    expect(mdkr_mod_registry_count(&reg) == 0, "and yields zero packs");
    expect(mdkr_mod_registry_skipped(&reg) == 0, "and reports nothing skipped");
    expect(reg.listing_incomplete == 0,
           "and is not reported as a folder that could not be read");
    mdkr_mod_registry_shutdown(&reg);
}

/* 4. Equal priorities are broken by case-insensitive directory name, so the
 * load order does not depend on the order the filesystem hands back. Both
 * packs here sort the other way round under a case-sensitive comparison, and
 * the other way round again by manifest name. */
static void test_equal_priority_ties_break_by_directory_name(const char *scratch) {
    char mods[1024];
    MdkrModRegistry reg;
    const MdkrModEntry *first;
    const MdkrModEntry *second;
    char resolved[MDKR_MOD_PATH_MAX];

    path2(mods, sizeof mods, scratch, "ties");
    write_pack_file(mods, "Zulu", "pack.ini", "[pack]\nname=Aardvark\npriority=50\n");
    write_pack_file(mods, "alpha", "pack.ini", "[pack]\nname=Zebra\npriority=50\n");
    write_pack_file(mods, "Zulu", "textures/tie.png", "Z");
    write_pack_file(mods, "alpha", "textures/tie.png", "a");

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "tied packs initialise");
    expect(mdkr_mod_registry_count(&reg) == 2, "both tied packs discovered");
    first = mdkr_mod_registry_entry(&reg, 0);
    second = mdkr_mod_registry_entry(&reg, 1);
    expect(first != NULL && strstr(first->root, "alpha") != NULL,
           "equal priority orders 'alpha' before 'Zulu' (case-insensitive)");
    expect(second != NULL && strstr(second->root, "Zulu") != NULL,
           "and 'Zulu' takes the later, winning slot");
    expect(mdkr_mod_registry_resolve(&reg, "textures/tie.png",
                                     resolved, sizeof resolved) == 1 &&
           strstr(resolved, "Zulu") != NULL,
           "the tie-break decides the collision deterministically");
    mdkr_mod_registry_shutdown(&reg);
}

/* 5. A directory with no pack.ini is reported, not silently dropped: it is
 * almost always a half-finished install the player wants told about. */
static void test_directory_without_manifest_is_reported(const char *scratch) {
    char mods[1024];
    char empty[1024];
    MdkrModRegistry reg;

    path2(mods, sizeof mods, scratch, "nomanifest");
    write_pack_file(mods, "real", "pack.ini", "[pack]\nname=Real\n");
    path3(empty, sizeof empty, mods, "forgot_the_ini", "textures/x.png");
    write_text_file(empty, "X");

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "registry still initialises");
    expect(mdkr_mod_registry_count(&reg) == 1, "the manifest-less directory is not a pack");
    expect(mdkr_mod_registry_skipped(&reg) == 1,
           "a directory with no pack.ini is reported, not silently ignored");
    expect(mdkr_mod_registry_skip_reason(&reg, 0) != NULL &&
           strstr(mdkr_mod_registry_skip_reason(&reg, 0), "pack.ini") != NULL,
           "and the reason says which file was missing");
    mdkr_mod_registry_shutdown(&reg);
}

/* 6. Dot entries are the filesystem's own bookkeeping, not content. */
static void test_dot_entries_are_skipped(const char *scratch) {
    char mods[1024];
    MdkrModRegistry reg;

    path2(mods, sizeof mods, scratch, "dotted");
    write_pack_file(mods, "visible", "pack.ini", "[pack]\nname=Visible\n");
    write_pack_file(mods, ".hidden", "pack.ini", "[pack]\nname=Hidden\n");
    write_pack_file(mods, ".DS_Store_dir", "pack.ini", "[pack]\nname=Junk\n");

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "dotted entries initialise");
    expect(mdkr_mod_registry_count(&reg) == 1,
           "an entry beginning with '.' is not a pack");
    expect(mdkr_mod_registry_skipped(&reg) == 0,
           "and is not reported as a broken one either");
    mdkr_mod_registry_shutdown(&reg);
}

/* 7. Traversal is refused before the filesystem is consulted at all. The bait
 * file below really exists one level above the pack root, so an unguarded
 * resolve would return it. */
static void test_path_traversal_is_rejected(const char *scratch) {
    char mods[1024];
    char bait[1024];
    MdkrModRegistry reg;
    char resolved[MDKR_MOD_PATH_MAX];

    path2(mods, sizeof mods, scratch, "traversal");
    write_pack_file(mods, "solo", "pack.ini", "[pack]\nname=Solo\n");
    write_pack_file(mods, "solo", "textures/inside.png", "I");
    path2(bait, sizeof bait, mods, "escape.png");
    write_text_file(bait, "X");

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "traversal fixture initialises");
    expect(mdkr_mod_registry_resolve(&reg, "textures/inside.png",
                                     resolved, sizeof resolved) == 1,
           "control: a path inside the pack resolves");

    expect(mdkr_mod_registry_resolve(&reg, "../escape.png",
                                     resolved, sizeof resolved) == 0,
           "'../escape.png' is rejected even though the file exists");
    expect(resolved[0] == '\0', "a rejected path leaves no output behind");
    expect(mdkr_mod_registry_resolve(&reg, "/etc/passwd",
                                     resolved, sizeof resolved) == 0,
           "'/etc/passwd' is rejected");
    expect(mdkr_mod_registry_resolve(&reg, "..\\windows",
                                     resolved, sizeof resolved) == 0,
           "'..\\windows' is rejected");
    expect(mdkr_mod_registry_resolve(&reg, "..",
                                     resolved, sizeof resolved) == 0,
           "a bare '..' is rejected");
    expect(mdkr_mod_registry_resolve(&reg, "C:\\Windows\\win.ini",
                                     resolved, sizeof resolved) == 0,
           "a drive-qualified path is rejected");
    expect(mdkr_mod_registry_resolve(&reg, "textures/../../escape.png",
                                     resolved, sizeof resolved) == 0,
           "a '..' buried mid-path is rejected too");
    mdkr_mod_registry_shutdown(&reg);
}

/* 8. Past the fixed capacity, the extra packs are reported. A player who
 * installs 70 packs must be told why six of them do nothing. */
static void test_pack_overflow_is_recorded(const char *scratch) {
    char mods[1024];
    MdkrModRegistry reg;
    char limit_text[32];
    int total = MDKR_MOD_MAX_PACKS + 6;
    int index;
    int found_limit_reason = 0;

    path2(mods, sizeof mods, scratch, "overflow");
    for (index = 0; index < total; index++) {
        char pack[32];
        char ini[64];
        snprintf(pack, sizeof pack, "pack%02d", index);
        snprintf(ini, sizeof ini, "[pack]\nname=Pack %02d\n", index);
        write_pack_file(mods, pack, "pack.ini", ini);
    }

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "an over-full mods dir initialises");
    expect(mdkr_mod_registry_count(&reg) == MDKR_MOD_MAX_PACKS,
           "the registry stops at its fixed capacity");
    expect(mdkr_mod_registry_skipped(&reg) == total - MDKR_MOD_MAX_PACKS,
           "every pack past the capacity is accounted for");

    snprintf(limit_text, sizeof limit_text, "%d", MDKR_MOD_MAX_PACKS);
    for (index = 0; index < mdkr_mod_registry_skipped(&reg); index++) {
        const char *reason = mdkr_mod_registry_skip_reason(&reg, index);
        if (reason != NULL && strstr(reason, limit_text) != NULL) found_limit_reason = 1;
    }
    expect(found_limit_reason,
           "the overflow is explained by the capacity, never a silent drop");
    mdkr_mod_registry_shutdown(&reg);
}

/* 8b. Past the capacity of the SKIP list -- a different overflow from case 8,
 * and the one where the report can start lying rather than merely stopping.
 *
 * The last slot is rewritten to say more went wrong than is listed. Settings ->
 * Content draws that list as "<name> — <reason>", so if only the reason is
 * rewritten the row shows one pack's name against another pack's explanation:
 * the screen a player opens to find out why a pack did not load, giving a
 * wrong answer, in the case where the most has already gone wrong. The
 * assertion is therefore about the two halves agreeing, not about the wording
 * of either. */
static void test_overflowing_skip_list_names_what_it_explains(const char *scratch) {
    char mods[1024];
    MdkrModRegistry reg;
    /* Enough unreadable packs to overrun the skip list, not merely fill it. */
    int total = MDKR_MOD_MAX_PACKS + 6;
    int index;
    const char *last_name;
    const char *last_reason;
    int names_a_fixture_pack = 0;

    path2(mods, sizeof mods, scratch, "skipoverflow");
    for (index = 0; index < total; index++) {
        char pack[32];
        snprintf(pack, sizeof pack, "skipme%02d", index);
        /* A directory with something in it but no pack.ini: skipped, and
         * skipped for a reason of its own that names the pack. */
        write_pack_file(mods, pack, "readme.txt", "not a manifest\n");
    }

    expect(mdkr_mod_registry_init(&reg, mods) == 0,
           "a mods dir with more unreadable packs than the skip list holds "
           "initialises");
    expect(mdkr_mod_registry_skipped(&reg) == MDKR_MOD_MAX_PACKS,
           "the skip list fills to its capacity and stops there");

    last_name = reg.skip_name[MDKR_MOD_MAX_PACKS - 1];
    last_reason = mdkr_mod_registry_skip_reason(&reg, MDKR_MOD_MAX_PACKS - 1);
    expect(last_reason != NULL && strstr(last_reason, "list is full") != NULL,
           "the last row explains that the list itself ran out");

    for (index = 0; index < total; index++) {
        char pack[32];
        snprintf(pack, sizeof pack, "skipme%02d", index);
        if (strcmp(last_name, pack) == 0) names_a_fixture_pack = 1;
    }
    expect(last_name[0] != '\0',
           "and that row still has a name to show beside it");
    expect(!names_a_fixture_pack,
           "which is not some installed pack's, left over from the row this "
           "one replaced");
    mdkr_mod_registry_shutdown(&reg);
}

/* 9. `enabled = 0` is a real off switch, not a hint. */
static void test_disabled_pack_never_wins(const char *scratch) {
    char mods[1024];
    MdkrModRegistry reg;
    char resolved[MDKR_MOD_PATH_MAX];

    path2(mods, sizeof mods, scratch, "disabled");
    write_pack_file(mods, "winner", "pack.ini", "[pack]\nname=Winner\npriority=10\n");
    write_pack_file(mods, "blocked", "pack.ini",
                    "[pack]\nname=Blocked\npriority=90\nenabled=0\n");
    write_pack_file(mods, "winner", "textures/shared.png", "W");
    write_pack_file(mods, "blocked", "textures/shared.png", "B");
    write_pack_file(mods, "blocked", "textures/only_here.png", "B");

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "disabled fixture initialises");
    expect(mdkr_mod_registry_count(&reg) == 2,
           "a disabled pack is still listed as installed");
    expect(mdkr_mod_registry_resolve(&reg, "textures/shared.png",
                                     resolved, sizeof resolved) == 1 &&
           strstr(resolved, "winner") != NULL,
           "a disabled pack does not win a collision it outranks");
    expect(mdkr_mod_registry_resolve(&reg, "textures/only_here.png",
                                     resolved, sizeof resolved) == 0,
           "and contributes no files of its own");
    mdkr_mod_registry_shutdown(&reg);
}

/* Writes a two-entry archive at `root/name`: pack.ini and one texture. */
static void write_zip_pack(const char *root, const char *name,
                           const char *manifest, const char *texture) {
    char full[1024];
    mz_zip_archive zip;

    path2(full, sizeof full, root, name);
    make_parent_directories(full);
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_file(&zip, full, 0) ||
        !mz_zip_writer_add_mem(&zip, "pack.ini", manifest, strlen(manifest),
                               MZ_BEST_SPEED) ||
        !mz_zip_writer_add_mem(&zip, texture, "Z", 1, MZ_BEST_SPEED) ||
        !mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        fatal("cannot write scratch archive", full);
    }
    mz_zip_writer_end(&zip);
}

/* 10. Every name a pack serves is indexed at scan (issue #66).
 *
 * The texture store asks "does an enabled pack hold textures/<digest>.png?"
 * for every new texture the game draws, and most answers are no. Asking the
 * pack itself cost one archive open -- a full central-directory parse -- per
 * question. The index answers from memory, so the property under test is that
 * the answer is the SCAN's: a file deleted after the scan is still reported,
 * which a lookup that touched the disk could not do. Priority, the enabled
 * bit and the name validator apply exactly as they do to open_file(). */
static void test_names_are_indexed_at_scan(const char *scratch) {
    static const char digest_name[] =
        "textures/0123456789abcdef0123456789abcdef.png";
    static const char zipped_name[] =
        "textures/fedcba9876543210fedcba9876543210.png";
    char mods[1024];
    char gone[1024];
    MdkrModRegistry reg;
    int low = -1, high = -1, off = -1, zipped = -1;
    int index;

    path2(mods, sizeof mods, scratch, "indexed");
    write_pack_file(mods, "low", "pack.ini", "[pack]\nname=Low\npriority=10\n");
    write_pack_file(mods, "high", "pack.ini", "[pack]\nname=High\npriority=20\n");
    write_pack_file(mods, "off", "pack.ini",
                    "[pack]\nname=Off\npriority=90\nenabled=0\n");
    write_pack_file(mods, "low", digest_name, "L");
    write_pack_file(mods, "high", digest_name, "H");
    write_pack_file(mods, "off", digest_name, "O");
    write_pack_file(mods, "off", "textures/only_off.png", "O");
    write_pack_file(mods, "low", "textures/only_low.png", "L");
    /* Not digest-named: nested one level too deep, and the wrong length. */
    write_pack_file(mods, "low",
                    "textures/nested/0123456789abcdef0123456789abcdef.png", "L");
    write_pack_file(mods, "high", "textures/0123.png", "H");
    write_pack_file(mods, "high", "music/track.ogg", "H");
    write_zip_pack(mods, "zipped.zip", "[pack]\nname=Zipped\npriority=30\n",
                   zipped_name);

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "indexed fixture initialises");
    for (index = 0; index < mdkr_mod_registry_count(&reg); index++) {
        const char *name = reg.entries[index].manifest.name;
        if (strcmp(name, "Low") == 0) low = index;
        else if (strcmp(name, "High") == 0) high = index;
        else if (strcmp(name, "Off") == 0) off = index;
        else if (strcmp(name, "Zipped") == 0) zipped = index;
    }
    if (low < 0 || high < 0 || off < 0 || zipped < 0) {
        expect(0, "all four indexed packs are discovered");
        mdkr_mod_registry_shutdown(&reg);
        return;
    }
    expect(reg.entries[low].names_indexed && reg.entries[high].names_indexed &&
           reg.entries[off].names_indexed && reg.entries[zipped].names_indexed,
           "every pack's names are indexed at scan, folder and zip alike");
    expect(mdkr_mod_registry_name_packs(&reg, digest_name) ==
               ((UINT64_C(1) << low) | (UINT64_C(1) << high) |
                (UINT64_C(1) << off)),
           "the index names every pack holding a file, enabled or not");
    expect(mdkr_mod_registry_find_file(&reg, digest_name) == high,
           "the highest-priority enabled holder wins, as in open_file()");
    expect(mdkr_mod_registry_find_file(&reg, zipped_name) == zipped,
           "a name inside a zip is found without opening the zip");
    expect(mdkr_mod_registry_find_file(&reg, "textures/only_off.png") == -1,
           "a disabled pack contributes no names");
    expect(mdkr_mod_registry_find_file(&reg, "textures/absent.png") == -1 &&
           mdkr_mod_registry_name_packs(&reg, "textures/absent.png") == 0,
           "a name no pack holds is absent");
    expect(mdkr_mod_registry_find_file(&reg, "../low/textures/only_low.png") == -1 &&
           mdkr_mod_registry_find_file(&reg, "TEXTURES/only_low.png") == -1,
           "the one name validator applies, and matching is case-sensitive");
    expect(reg.entries[low].digest_textures == 1 &&
           reg.entries[high].digest_textures == 1 &&
           reg.entries[zipped].digest_textures == 1,
           "only textures/<32 characters>.png counts as a digest-named texture");

    /* The answer is the scan's, not the filesystem's. */
    path3(gone, sizeof gone, mods, "low", "textures/only_low.png");
    expect(remove(gone) == 0, "the fixture file can be deleted after the scan");
    expect(mdkr_mod_registry_find_file(&reg, "textures/only_low.png") == low,
           "and the index still answers from the scan, with no file I/O");

    mdkr_mod_registry_shutdown(&reg);
    expect(mdkr_mod_registry_find_file(&reg, digest_name) == -1 &&
           mdkr_mod_registry_name_packs(&reg, digest_name) == 0,
           "a shut-down registry indexes nothing");
}

static int same_ignoring_case(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

/* 11. Digest-named textures match whatever their case (#66 fix round 1).
 *
 * 1.7.0 found textures/<digest>.png by asking the filesystem, which on macOS
 * and Windows ignores case: a folder pack spelling it Textures/ABC...PNG
 * worked there. The scan-time index must not quietly drop that pack, so a
 * name that is a content digest in everything but case is indexed under its
 * lowercase form and remembers how the pack spells it -- in a folder pack and
 * a zip alike. Nothing else changes case: other names, Rice ones included,
 * still match exactly. A file under textures/ that can never be used (not
 * digest-shaped) is counted and named, so the scan can say why. */
static void test_digest_names_ignore_case(const char *scratch) {
    static const char folder_lower[] =
        "textures/abcdef0123456789abcdef0123456789.png";
    static const char folder_spelled[] =
        "Textures/ABCDEF0123456789ABCDEF0123456789.PNG";
    static const char zip_lower[] =
        "textures/0011223344556677889900aabbccddee.png";
    static const char zip_spelled[] =
        "TEXTURES/0011223344556677889900AABBCCDDEE.Png";
    char mods[1024];
    MdkrModRegistry reg;
    MdkrModNameHit hits[4];
    int folder = -1, zipped = -1, index, count;

    path2(mods, sizeof mods, scratch, "case");
    write_pack_file(mods, "upper", "pack.ini", "[pack]\nname=Upper\n");
    write_pack_file(mods, "upper", folder_spelled, "U");
    write_pack_file(mods, "upper", "textures/not-a-digest.png", "U");
    write_pack_file(mods, "upper",
                    "Textures/nested/0123456789abcdef0123456789abcdef.png", "U");
    write_pack_file(mods, "upper", "music/Track.ogg", "U");
    write_zip_pack(mods, "upperzip.zip", "[pack]\nname=UpperZip\n", zip_spelled);

    expect(mdkr_mod_registry_init(&reg, mods) == 0, "case fixture initialises");
    for (index = 0; index < mdkr_mod_registry_count(&reg); index++) {
        if (strcmp(reg.entries[index].manifest.name, "Upper") == 0) folder = index;
        if (strcmp(reg.entries[index].manifest.name, "UpperZip") == 0) zipped = index;
    }
    if (folder < 0 || zipped < 0) {
        expect(0, "both case packs are discovered");
        mdkr_mod_registry_shutdown(&reg);
        return;
    }
    expect(mdkr_mod_registry_find_file(&reg, folder_lower) == folder &&
           mdkr_mod_registry_find_file(&reg, folder_spelled) == folder,
           "a folder pack's Textures/<UPPER>.PNG answers the lowercase digest");
    count = mdkr_mod_registry_name_hits(&reg, folder_lower, hits, 4);
    expect(count == 1 && hits[0].packs == (UINT64_C(1) << folder) &&
           strcmp(hits[0].name, folder_spelled) == 0,
           "and the index hands back the pack's own spelling to read it by");
    count = mdkr_mod_registry_name_hits(&reg, zip_lower, hits, 4);
    expect(mdkr_mod_registry_find_file(&reg, zip_lower) == zipped &&
           count == 1 && strcmp(hits[0].name, zip_spelled) == 0,
           "a zip's TEXTURES/<UPPER>.Png answers it too, with its spelling");
    expect(reg.entries[folder].digest_textures == 1 &&
           reg.entries[zipped].digest_textures == 1,
           "both count as digest-named textures");
    expect(mdkr_mod_registry_find_file(&reg, "MUSIC/track.ogg") == -1 &&
           mdkr_mod_registry_find_file(&reg, "music/Track.ogg") == folder,
           "a name that is not a digest still matches exactly");
    /* Compared ignoring case: on a filesystem that ignores case (macOS,
     * Windows) the fixture's Textures/ and textures/ are one directory, so
     * the scan reports whichever spelling the directory was created with. */
    expect(reg.entries[folder].textures_skipped == 2 &&
           (same_ignoring_case(reg.entries[folder].textures_skipped_first,
                               "textures/not-a-digest.png") ||
            same_ignoring_case(
                reg.entries[folder].textures_skipped_first,
                "textures/nested/0123456789abcdef0123456789abcdef.png")),
           "files under textures/ that can never be used are counted and named");
    expect(reg.entries[zipped].textures_skipped == 0,
           "and a pack with none reports none");
    mdkr_mod_registry_shutdown(&reg);
}

/* Writes an archive at `root/name` holding pack.ini (when `manifest` is not
 * NULL) and then every name in `names`, in that order, each a one-byte file.
 * That order is the central directory's, which is the order the scan indexes
 * a zip in. */
static void write_zip_names(const char *root, const char *name,
                            const char *manifest, const char *const *names,
                            size_t count) {
    char           full[1024];
    mz_zip_archive zip;
    size_t         index;
    int            ok;

    path2(full, sizeof full, root, name);
    make_parent_directories(full);
    memset(&zip, 0, sizeof zip);
    ok = mz_zip_writer_init_file(&zip, full, 0);
    if (ok && manifest != NULL) {
        ok = mz_zip_writer_add_mem(&zip, "pack.ini", manifest, strlen(manifest),
                                   MZ_BEST_SPEED);
    }
    for (index = 0; ok && index < count; index++) {
        ok = mz_zip_writer_add_mem(&zip, names[index], "Z", 1, MZ_BEST_SPEED);
    }
    if (!ok || !mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        fatal("cannot write scratch archive", full);
    }
    mz_zip_writer_end(&zip);
}

/* `textures/<digest>.png` with the hex digits and the extension upper-cased:
 * the spelling a hand-renamed file has. */
static void upper_digest_name(char *out, size_t size, const char *lower) {
    size_t index;
    if (strlen(lower) + 1 > size) fatal("digest name too long", lower);
    for (index = 0; lower[index] != '\0'; index++) {
        char c = lower[index];
        out[index] = (index >= 9 && c >= 'a' && c <= 'z')
                         ? (char)(c - 'a' + 'A') : c;
    }
    out[index] = '\0';
}

#ifndef _WIN32
/* Runs a scan with stderr sent to `log`, so a case can count the [MODS]
 * notes the scan printed. POSIX only; the Windows build prints a SKIP. */
static int init_capturing_stderr(MdkrModRegistry *reg, const char *mods,
                                 const char *log) {
    int saved;
    int fd;
    int result;

    fflush(stderr);
    saved = dup(2);
    fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (saved < 0 || fd < 0) fatal("cannot capture stderr", log);
    dup2(fd, 2);
    close(fd);
    result = mdkr_mod_registry_init(reg, mods);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    return result;
}

static int count_in_file(const char *path, const char *needle) {
    static char text[65536];
    FILE       *file = fopen(path, "rb");
    size_t      length;
    int         count = 0;
    const char *cursor;

    if (file == NULL) return -1;
    length = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[length] = '\0';
    for (cursor = strstr(text, needle); cursor != NULL;
         cursor = strstr(cursor + 1, needle)) {
        count++;
    }
    return count;
}
#endif

/* The one spelling `pack` is read by for `lower`, or NULL when the index
 * lists it under none or under more than one. */
static const char *only_spelling(const MdkrModRegistry *reg, const char *lower,
                                 int pack) {
    MdkrModNameHit hits[MDKR_MOD_MAX_PACKS];
    const char    *found = NULL;
    const int      count = mdkr_mod_registry_name_hits(reg, lower, hits,
                                                       MDKR_MOD_MAX_PACKS);
    int            index;

    for (index = 0; index < count; index++) {
        if ((hits[index].packs & (UINT64_C(1) << pack)) == 0) continue;
        if (found != NULL) return NULL;
        found = hits[index].name;
    }
    return found;
}

/* 15. One pack holding a digest twice, spelled differently only in case
 * (T12d). The index is keyed by the lowercase form, so both spellings land
 * under one key, and which one the store read used to depend on hash-probe
 * and scan order -- which a table grow reshuffles. The rule is now fixed: the
 * exact lowercase spelling (the name 1.7.0 asked for), otherwise the first in
 * byte order. Each pack is listed under exactly one spelling, and the scan
 * prints one [MODS] note per duplicate it set aside.
 *
 * The zip holds three pairs in different orders around 800 filler names,
 * which force the name table to grow mid-pack: a pair indexed before the
 * grow, a pair split across it each way. A folder pack can hold both
 * spellings only on a case-sensitive file system; elsewhere that arm prints
 * a SKIP (on macOS run the binary with a scratch root on a case-sensitive
 * volume to exercise it). */
#define CASE_DUP_FILLERS 800
static void test_case_duplicates_prefer_lowercase(const char *scratch) {
    static const char d1[] = "textures/aaaa00000000000000000000000000bb.png";
    static const char d2[] = "textures/bbbb00000000000000000000000000cc.png";
    static const char d3[] = "textures/cccc00000000000000000000000000dd.png";
    static const char d4[] = "textures/dddd00000000000000000000000000ee.png";
    static const char d4_dir[] = "Textures/dddd00000000000000000000000000ee.png";
    static const char df[] = "textures/eeee00000000000000000000000000ff.png";
    char        u1[64], u2[64], u3[64], u4[64], uf[64];
    char        mods[1024];
    char        probe[1024];
    char        log[1024];
    const char *names[8 + CASE_DUP_FILLERS];
    char      (*fillers)[32];
    size_t      count = 0;
    MdkrModRegistry reg;
    int         zipped = -1, folder = -1, index;
    int         folder_arm;
    struct stat status;

    upper_digest_name(u1, sizeof u1, d1);
    upper_digest_name(u2, sizeof u2, d2);
    upper_digest_name(u3, sizeof u3, d3);
    upper_digest_name(u4, sizeof u4, d4);
    upper_digest_name(uf, sizeof uf, df);
    fillers = (char (*)[32])calloc(CASE_DUP_FILLERS, sizeof *fillers);
    if (fillers == NULL) fatal("out of memory", "fillers");
    for (index = 0; index < CASE_DUP_FILLERS; index++) {
        snprintf(fillers[index], sizeof fillers[index], "other/%04d.bin", index);
    }
    names[count++] = u1;        /* d1: upper then lower, both before the grow */
    names[count++] = d1;
    names[count++] = d2;        /* d2: lower before the grow, upper after */
    names[count++] = u3;        /* d3: upper before the grow, lower after */
    names[count++] = u4;        /* d4: two spellings, neither lowercase */
    names[count++] = d4_dir;
    for (index = 0; index < CASE_DUP_FILLERS; index++) {
        names[count++] = fillers[index];
    }
    names[count++] = u2;
    names[count++] = d3;

    path2(mods, sizeof mods, scratch, "casedup");
    write_zip_names(mods, "dupzip.zip", "[pack]\nname=DupZip\npriority=20\n",
                    names, count);
    write_pack_file(mods, "dupfolder", "pack.ini",
                    "[pack]\nname=DupFolder\npriority=10\n");
    write_pack_file(mods, "dupfolder", uf, "U");
    path3(probe, sizeof probe, mods, "dupfolder", df);
    folder_arm = stat(probe, &status) != 0;
    if (folder_arm) {
        write_pack_file(mods, "dupfolder", df, "L");
    } else {
        printf("SKIP this file system ignores case, so a folder cannot hold "
               "both spellings\n");
    }

    path2(log, sizeof log, scratch, "casedup.log");
#ifndef _WIN32
    expect(init_capturing_stderr(&reg, mods, log) == 0,
           "case-duplicate fixture initialises");
#else
    expect(mdkr_mod_registry_init(&reg, mods) == 0,
           "case-duplicate fixture initialises");
#endif
    for (index = 0; index < mdkr_mod_registry_count(&reg); index++) {
        if (strcmp(reg.entries[index].manifest.name, "DupZip") == 0) zipped = index;
        if (strcmp(reg.entries[index].manifest.name, "DupFolder") == 0) folder = index;
    }
    if (zipped < 0 || folder < 0) {
        expect(0, "both case-duplicate packs are discovered");
        mdkr_mod_registry_shutdown(&reg);
        free(fillers);
        return;
    }
    expect(reg.name_capacity > 1024u,
           "the fillers made the name table grow mid-pack");
    expect(only_spelling(&reg, d1, zipped) != NULL &&
           strcmp(only_spelling(&reg, d1, zipped), d1) == 0,
           "a zip holding UPPER then lower is read by the lowercase name");
    expect(only_spelling(&reg, d2, zipped) != NULL &&
           strcmp(only_spelling(&reg, d2, zipped), d2) == 0,
           "and lower, then UPPER after a table grow, still by the lowercase");
    expect(only_spelling(&reg, d3, zipped) != NULL &&
           strcmp(only_spelling(&reg, d3, zipped), d3) == 0,
           "and UPPER, then lower after a table grow, still by the lowercase");
    expect(only_spelling(&reg, d4, zipped) != NULL &&
           strcmp(only_spelling(&reg, d4, zipped), d4_dir) == 0,
           "with no lowercase spelling, the first in byte order wins");
    expect(mdkr_mod_registry_find_file(&reg, d1) == zipped &&
           mdkr_mod_registry_find_file(&reg, u2) == zipped,
           "the pack still answers for the digest in any case");
    expect(reg.entries[zipped].textures_case_duplicates == 4,
           "each duplicate set aside is counted against its pack");
    if (folder_arm) {
        expect(only_spelling(&reg, df, folder) != NULL &&
               strcmp(only_spelling(&reg, df, folder), df) == 0 &&
               reg.entries[folder].textures_case_duplicates == 1,
               "a folder pack holding both is read by the lowercase name too");
    }
#ifndef _WIN32
    expect(count_in_file(log, "differ only in case") ==
               4 + (folder_arm ? 1 : 0),
           "the scan prints one [MODS] note per duplicate");
#else
    printf("SKIP the scan's notes are not captured on Windows\n");
#endif
    mdkr_mod_registry_shutdown(&reg);

    /* The same rule with no grow at all: a two-name zip, upper first. */
    path2(mods, sizeof mods, scratch, "casedup-small");
    names[0] = u1;
    names[1] = d1;
    write_zip_names(mods, "small.zip", "[pack]\nname=Small\n", names, 2);
    expect(mdkr_mod_registry_init(&reg, mods) == 0 &&
               mdkr_mod_registry_count(&reg) == 1,
           "the small case-duplicate fixture initialises");
    expect(only_spelling(&reg, d1, 0) != NULL &&
           strcmp(only_spelling(&reg, d1, 0), d1) == 0 &&
           reg.name_capacity == 1024u,
           "and without a grow the lowercase name wins the same way");
    mdkr_mod_registry_shutdown(&reg);
    free(fillers);
}

/* 16. More spellings of one digest than the store used to ask for (T12d).
 * Nine packs each spell textures/<digest>.png their own way; the index then
 * holds nine slots under one key, inserted lowest priority first. The store
 * asked for eight hits, so the ninth -- the highest-priority pack's -- was
 * dropped without a word and a lower pack's picture was drawn instead. Hits
 * now come back highest priority first, at most one per pack, so a caller
 * asking for MDKR_MOD_MAX_PACKS never truncates and one asking for fewer
 * keeps the packs that win. */
static void test_name_hits_keep_the_winning_pack(const char *scratch) {
    static const char lower[] = "textures/abcdefabcdefabcdefabcdefabcdefab.png";
    char            mods[1024];
    char            spellings[9][64];
    MdkrModNameHit  hits[MDKR_MOD_MAX_PACKS];
    MdkrModRegistry reg;
    int             top = -1, index, count, ordered = 1;

    path2(mods, sizeof mods, scratch, "manyspellings");
    for (index = 0; index < 9; index++) {
        char pack[16];
        char manifest[64];
        snprintf(pack, sizeof pack, "p%d", index);
        snprintf(manifest, sizeof manifest, "[pack]\nname=P%d\npriority=%d\n",
                 index, 10 * (index + 1));
        /* Pack i upper-cases the i-th hex letter: nine distinct spellings. */
        memcpy(spellings[index], lower, sizeof lower);
        spellings[index][9 + index] =
            (char)(spellings[index][9 + index] - 'a' + 'A');
        write_pack_file(mods, pack, "pack.ini", manifest);
        write_pack_file(mods, pack, spellings[index], "X");
    }
    expect(mdkr_mod_registry_init(&reg, mods) == 0 &&
               mdkr_mod_registry_count(&reg) == 9,
           "nine spelling packs are discovered");
    for (index = 0; index < mdkr_mod_registry_count(&reg); index++) {
        if (strcmp(reg.entries[index].manifest.name, "P8") == 0) top = index;
    }
    count = mdkr_mod_registry_name_hits(&reg, lower, hits, MDKR_MOD_MAX_PACKS);
    expect(count == 9, "every pack's spelling is listed");
    for (index = 1; index < count; index++) {
        if (hits[index - 1].packs <= hits[index].packs) ordered = 0;
    }
    expect(top >= 0 && count > 0 &&
           hits[0].packs == (UINT64_C(1) << top) &&
           strcmp(hits[0].name, spellings[8]) == 0 && ordered,
           "highest priority first");
    count = mdkr_mod_registry_name_hits(&reg, lower, hits, 1);
    expect(count == 1 && top >= 0 && hits[0].packs == (UINT64_C(1) << top) &&
           strcmp(hits[0].name, spellings[8]) == 0,
           "a caller asking for fewer keeps the winning pack, not the first "
           "one indexed");
    mdkr_mod_registry_shutdown(&reg);
}

#ifdef MDKR_FS_UTF8_TESTING
/* fs_utf8.c's test seam: at 0 or more a collected listing stops with EIO once
 * it holds that many names, as a read error part-way through would. */
extern int mdkr_fs_utf8_test_names_fail_after;
#endif

/* 13. A mods folder that exists but cannot be listed completely is not "no
 * mods folder": the packs read before the failure are kept, and the scan says
 * the folder could not be read (a [MODS] line on stderr). 1.7.0's readdir()
 * loop kept them too. */
static void test_listing_failure_keeps_what_was_read(const char *scratch) {
    char mods[1024];
    char not_a_folder[1024];
    MdkrModRegistry reg;

    path2(mods, sizeof mods, scratch, "partial");
    write_pack_file(mods, "one", "pack.ini", "[pack]\nname=One\n");
    write_pack_file(mods, "two", "pack.ini", "[pack]\nname=Two\n");
    write_pack_file(mods, "three", "pack.ini", "[pack]\nname=Three\n");
#ifdef MDKR_FS_UTF8_TESTING
    mdkr_fs_utf8_test_names_fail_after = 2;
    expect(mdkr_mod_registry_init(&reg, mods) == 0,
           "a listing that fails part-way does not fail the scan");
    expect(mdkr_mod_registry_count(&reg) == 2,
           "the packs read before the failure are kept");
    expect(reg.listing_incomplete == 1,
           "and the folder is reported as not read completely");
    mdkr_mod_registry_shutdown(&reg);
    mdkr_fs_utf8_test_names_fail_after = 0;
    expect(mdkr_mod_registry_init(&reg, mods) == 0 &&
               mdkr_mod_registry_count(&reg) == 0 && reg.listing_incomplete == 1,
           "a folder that cannot be listed at all is reported, not taken for "
           "a missing one");
    mdkr_mod_registry_shutdown(&reg);
    mdkr_fs_utf8_test_names_fail_after = -1;
#else
    printf("SKIP the listing-failure seam needs MDKR_FS_UTF8_TESTING\n");
#endif
    expect(mdkr_mod_registry_init(&reg, mods) == 0 &&
               mdkr_mod_registry_count(&reg) == 3 && reg.listing_incomplete == 0,
           "the same folder listed completely loads every pack");
    mdkr_mod_registry_shutdown(&reg);

    path2(not_a_folder, sizeof not_a_folder, scratch, "mods-is-a-file");
    write_text_file(not_a_folder, "not a folder");
    expect(mdkr_mod_registry_init(&reg, not_a_folder) == 0 &&
               mdkr_mod_registry_count(&reg) == 0 && reg.listing_incomplete == 1,
           "a mods path held by a file is reported as unreadable, not missing");
    mdkr_mod_registry_shutdown(&reg);
}

/* 14. A pack whose name has no UTF-8 spelling is never loaded, but it is
 * listed as skipped under a readable stand-in with what to do, never dropped
 * without a word. Needs a file system that holds such names (Linux); macOS
 * refuses them, and the narrow calls this fixture uses cannot spell the
 * Windows equivalent, so both print a SKIP line. */
static void test_name_without_utf8_is_reported(const char *scratch) {
#ifndef _WIN32
    char mods[1024];
    char bad[1024];
    char manifest[1100];
    MdkrModRegistry reg;
    FILE *probe;

    path2(mods, sizeof mods, scratch, "unnamed");
    write_pack_file(mods, "good", "pack.ini", "[pack]\nname=Good\n");
    path2(bad, sizeof bad, mods, "lost-\xff-pack");
    if (TEST_MKDIR(bad) != 0) {
        printf("SKIP this file system cannot hold a name with no UTF-8 spelling\n");
        return;
    }
    path2(manifest, sizeof manifest, mods, "lost-\xEF\xBF\xBD-pack");
    if (is_directory(manifest)) {
        /* The file system rewrote the name (Emscripten's MEMFS keeps every
         * name as UTF-8, U+FFFD in place of what has none). */
        printf("SKIP this file system rewrote a name with no UTF-8 spelling\n");
        return;
    }
    path2(manifest, sizeof manifest, bad, "pack.ini");
    probe = fopen(manifest, "wb");
    if (probe == NULL) fatal("cannot create scratch file", manifest);
    fputs("[pack]\nname=Lost\n", probe);
    fclose(probe);
    expect(mdkr_mod_registry_init(&reg, mods) == 0 &&
               mdkr_mod_registry_count(&reg) == 1,
           "a pack whose name has no UTF-8 spelling does not stop the others");
    expect(mdkr_mod_registry_skipped(&reg) == 1 &&
               strcmp(reg.skip_name[0], "lost-\xEF\xBF\xBD" "-pack") == 0 &&
               strstr(mdkr_mod_registry_skip_reason(&reg, 0),
                      "not valid Unicode") != NULL,
           "it is listed as skipped under a readable stand-in, with what to do");
    mdkr_mod_registry_shutdown(&reg);
#else
    (void)scratch;
    printf("SKIP the narrow fixture calls cannot spell a name with no UTF-8 "
           "spelling here\n");
#endif
}

int main(int argc, char **argv) {
    const char *scratch = argc > 1 ? argv[1] : "mod_registry_scratch";

    remove_tree(scratch);

    test_priority_orders_packs(scratch);
    test_malformed_manifest_isolates(scratch);
    test_missing_mods_dir_is_not_an_error();
    test_equal_priority_ties_break_by_directory_name(scratch);
    test_directory_without_manifest_is_reported(scratch);
    test_dot_entries_are_skipped(scratch);
    test_path_traversal_is_rejected(scratch);
    test_pack_overflow_is_recorded(scratch);
    test_overflowing_skip_list_names_what_it_explains(scratch);
    test_disabled_pack_never_wins(scratch);
    test_names_are_indexed_at_scan(scratch);
    test_digest_names_ignore_case(scratch);
    test_case_duplicates_prefer_lowercase(scratch);
    test_name_hits_keep_the_winning_pack(scratch);
    test_listing_failure_keeps_what_was_read(scratch);
    test_name_without_utf8_is_reported(scratch);

    remove_tree(scratch);

    printf(failures ? "FAILURES: %d\n" : "all registry assertions passed\n", failures);
    return failures ? 1 : 0;
}
