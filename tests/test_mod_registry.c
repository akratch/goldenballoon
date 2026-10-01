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

    remove_tree(scratch);

    printf(failures ? "FAILURES: %d\n" : "all registry assertions passed\n", failures);
    return failures ? 1 : 0;
}
