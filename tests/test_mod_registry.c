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

#include <dirent.h>
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
    test_disabled_pack_never_wins(scratch);

    remove_tree(scratch);

    printf(failures ? "FAILURES: %d\n" : "all registry assertions passed\n", failures);
    return failures ? 1 : 0;
}
